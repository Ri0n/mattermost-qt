#include "ChannelTimelineController.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>

#include <QAbstractItemView>
#include <QDateTime>
#include <QEvent>
#include <QIcon>
#include <QListWidgetItem>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>

#include "ChatArea.h"
#include "PostsListWidget.h"
#include "backend/Backend.h"
#include "backend/PostTimelineService.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "post/PostWidget.h"
#include "ui_ChatArea.h"

namespace Mattermost {
namespace {

constexpr int ChannelPageSize = 30;
constexpr int SeekDebounceMs = 120;
constexpr int MeasurementDebounceMs = 180;
constexpr int GapPrefetchRows = 5;
constexpr int ContextPostsPerSide = 15;
constexpr int LoadingIndicatorIntervalMs = 80;
constexpr int LoadingIndicatorFrames = 12;
constexpr auto LoadingIndicatorObjectName = "timelineLoadingIndicatorTimer";

QIcon loadingIndicatorIcon(const QWidget* widget, int frame)
{
    constexpr int IconSize = 16;
    QPixmap pixmap(IconSize, IconSize);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QColor color = widget
        ? widget->palette().color(QPalette::ButtonText)
        : QColor(Qt::black);
    QPen pen(color, 2.0, Qt::SolidLine, Qt::RoundCap);
    painter.setPen(pen);

    const QRectF arcRect(2.5, 2.5, IconSize - 5.0, IconSize - 5.0);
    const int startAngle = (90 - (frame % LoadingIndicatorFrames) * 30) * 16;
    painter.drawArc(arcRect, startAngle, 250 * 16);
    return QIcon(pixmap);
}

QTimer* ensureLoadingIndicatorTimer(QPushButton* button)
{
    if (!button) {
        return nullptr;
    }

    if (auto* existing = button->findChild<QTimer*>(
            QLatin1String(LoadingIndicatorObjectName), Qt::FindDirectChildrenOnly)) {
        return existing;
    }

    auto* timer = new QTimer(button);
    timer->setObjectName(QLatin1String(LoadingIndicatorObjectName));
    timer->setInterval(LoadingIndicatorIntervalMs);
    timer->setProperty("frame", 0);
    QObject::connect(timer, &QTimer::timeout, button, [button, timer] {
        const int frame = (timer->property("frame").toInt() + 1)
            % LoadingIndicatorFrames;
        timer->setProperty("frame", frame);
        button->setIcon(loadingIndicatorIcon(button, frame));
    });
    return timer;
}

void setLoadingIndicator(QPushButton* button, bool loading)
{
    if (!button) {
        return;
    }

    QTimer* timer = ensureLoadingIndicatorTimer(button);
    if (!timer) {
        return;
    }

    if (!loading) {
        timer->stop();
        timer->setProperty("frame", 0);
        button->setIcon(QIcon());
        return;
    }

    button->setIconSize(QSize(16, 16));
    button->setIcon(loadingIndicatorIcon(button, timer->property("frame").toInt()));
    if (!timer->isActive()) {
        timer->start();
    }
}

QStringList uniqueChronologicalRootIds(const BackendChannel& channel,
                                       const QStringList& candidateIds)
{
    QVector<BackendPost*> posts;
    QSet<QString> seen;
    posts.reserve(candidateIds.size());

    for (const QString& id : candidateIds) {
        if (id.isEmpty() || seen.contains(id)) {
            continue;
        }
        BackendPost* post = channel.postIdToPost.value(id, nullptr);
        if (!post || post->hidden || !post->root_id.isEmpty()) {
            continue;
        }
        seen.insert(id);
        posts.push_back(post);
    }

    std::sort(posts.begin(), posts.end(), [](const BackendPost* lhs, const BackendPost* rhs) {
        if (lhs->create_at != rhs->create_at) {
            return lhs->create_at < rhs->create_at;
        }
        return lhs->id < rhs->id;
    });

    QStringList result;
    result.reserve(posts.size());
    for (const BackendPost* post : posts) {
        result.push_back(post->id);
    }
    return result;
}

} // namespace

ChannelTimelineController* createChannelTimelineController(ChatArea& area)
{
    if (area.isThread) {
        return nullptr;
    }
    return new ChannelTimelineController(area);
}

ChannelTimelineController::ChannelTimelineController(ChatArea& sourceArea)
    : QObject(&sourceArea)
    , area(sourceArea)
{
    seekTimer.setSingleShot(true);
    seekTimer.setInterval(SeekDebounceMs);
    connect(&seekTimer, &QTimer::timeout, this, [this] {
        if (pendingSeekIndex >= 0) {
            requestSeek(pendingSeekIndex);
        }
    });

    measurementTimer.setSingleShot(true);
    measurementTimer.setInterval(MeasurementDebounceMs);
    connect(&measurementTimer, &QTimer::timeout,
            this, &ChannelTimelineController::measureRenderedPosts);

    area.installEventFilter(this);
    QTimer::singleShot(0, this, &ChannelTimelineController::tryStart);
}

bool ChannelTimelineController::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != &area) {
        return QObject::eventFilter(watched, event);
    }

    if (event->type() == QEvent::Show) {
        QTimer::singleShot(0, this, &ChannelTimelineController::tryStart);
    } else if (event->type() == QEvent::Hide) {
        QPointer<ChannelTimelineController> guard(this);
        QTimer::singleShot(0, this, [guard] {
            if (!guard || !guard->active) {
                return;
            }
            BackendChannel* current = guard->area.backend.getCurrentChannel();
            if (current && current != &guard->area.channel) {
                guard->deactivate();
                return;
            }
            if (!guard->area.isVisible()) {
                QTimer::singleShot(50, guard, [guard] {
                    if (!guard || !guard->active || guard->area.isVisible()) {
                        return;
                    }
                    BackendChannel* delayedCurrent = guard->area.backend.getCurrentChannel();
                    if (delayedCurrent && delayedCurrent != &guard->area.channel) {
                        guard->deactivate();
                    }
                });
            }
        });
    }
    return QObject::eventFilter(watched, event);
}

void ChannelTimelineController::tryStart()
{
    if (active || area.isThread || !area.ui || !area.ui->listWidget) {
        return;
    }

    if (!area.initialized) {
        if (area.isVisible() || area.backend.getCurrentChannel() == &area.channel) {
            QTimer::singleShot(10, this, &ChannelTimelineController::tryStart);
        }
        return;
    }

    BackendChannel* current = area.backend.getCurrentChannel();
    if (!area.isVisible() && current && current != &area.channel) {
        return;
    }
    start();
}

int ChannelTimelineController::channelRootPostCount() const
{
    if (!area.channel.has_total_msg_count_root) {
        return 0;
    }
    return std::max(0, area.channel.total_msg_count_root);
}

void ChannelTimelineController::start()
{
    if (active) {
        return;
    }

    active = true;
    ++generation;
    ++pruneGeneration;
    requestInFlight = false;
    requestedPage = -1;
    requestedFocusIndex = -1;
    pendingSeekIndex = -1;
    viewportCheckScheduled = false;
    loadedPages.clear();
    deferredExternalPostIds.clear();
    nextOlderPage = 0;
    initialRenderDone = false;
    lastUserViewportAnchor = ViewportAnchor();
    contextNavigationActive = false;
    contextNavigationPostId.clear();
    contextOldestPostId.clear();
    contextNewestPostId.clear();
    contextReachedOldest = false;
    contextReachedNewest = false;

    totalCountExact = area.channel.has_total_msg_count_root;
    expectedPostCount = channelRootPostCount();
    timeline.reset(expectedPostCount);
    lastAppliedGapRowHeight = timeline.estimatedRowHeight();

    QObject::disconnect(&area.channel, &BackendChannel::onNewPosts,
                        &area, &ChatArea::fillChannelPosts);
    area.gettingOlderPosts = true;
    setLoadingIndicator(area.ui->loadOldPosts, false);

    connect(area.ui->loadOldPosts, &QPushButton::clicked,
            this, &ChannelTimelineController::requestOlderPage);

    QScrollBar* scrollBar = area.ui->listWidget->verticalScrollBar();
    connect(scrollBar, &QScrollBar::valueChanged, this,
            [this](int) { scheduleViewportCheck(); });
    connect(scrollBar, &QScrollBar::sliderReleased, this,
            [this] { scheduleViewportCheck(); });
    connect(area.ui->listWidget, &PostsListWidget::userViewportChanged, this,
            [this](bool) {
        lastUserViewportAnchor = captureViewportAnchor();
        scheduleViewportCheck();
        schedulePrune();
    });

    connect(&area.channel, &BackendChannel::onNewPosts, this,
            &ChannelTimelineController::absorbNewPosts);

    connect(&area.channel, &BackendChannel::onNewPost, this,
            [this](BackendPost& post) {
        if (!active || post.hidden || !post.root_id.isEmpty()) {
            return;
        }
        ++expectedPostCount;
        timeline.setTotalCount(expectedPostCount);
        timeline.placeWindow(expectedPostCount - 1, QStringList {post.id});
        scheduleMeasurementPass();
    });

    initialPageTarget = 1;
    continueInitialPrefetch();
}

void ChannelTimelineController::beginContextNavigation(const QString& postId)
{
    if (!active || postId.isEmpty()) {
        return;
    }

    contextNavigationActive = true;
    contextNavigationPostId = postId;
    contextOldestPostId.clear();
    contextNewestPostId.clear();
    contextReachedOldest = false;
    contextReachedNewest = false;
    lastUserViewportAnchor = ViewportAnchor();
    pendingSeekIndex = -1;
    seekTimer.stop();

    const int logicalIndex = timeline.indexOf(postId);
    if (logicalIndex < 0) {
        return;
    }
    for (const PostTimeline::Span& span : timeline.spans()) {
        if (span.kind != PostTimeline::LoadedSpan
            || logicalIndex < span.firstIndex
            || logicalIndex >= span.firstIndex + span.count
            || span.postIds.isEmpty()) {
            continue;
        }
        contextOldestPostId = span.postIds.first();
        contextNewestPostId = span.postIds.last();
        break;
    }
}

void ChannelTimelineController::deactivate()
{
    if (!active) {
        return;
    }

    active = false;
    ++generation;
    ++renderGeneration;
    ++pruneGeneration;
    seekTimer.stop();
    measurementTimer.stop();
    pendingSeekIndex = -1;
    requestInFlight = false;
    contextNavigationActive = false;
    contextNavigationPostId.clear();
    contextOldestPostId.clear();
    contextNewestPostId.clear();
    lastUserViewportAnchor = ViewportAnchor();
    area.gettingOlderPosts = false;

    QObject::disconnect(&area.channel, nullptr, this, nullptr);
    if (area.ui) {
        if (area.ui->loadOldPosts) {
            setLoadingIndicator(area.ui->loadOldPosts, false);
            QObject::disconnect(area.ui->loadOldPosts, nullptr, this, nullptr);
        }
        if (area.ui->listWidget) {
            area.ui->listWidget->setUpdatesEnabled(true);
            QObject::disconnect(area.ui->listWidget, nullptr, this, nullptr);
            QObject::disconnect(area.ui->listWidget->verticalScrollBar(), nullptr, this, nullptr);
        }
    }
}

int ChannelTimelineController::authoritativeFirstIndex(int page, int pageSize) const
{
    if (page < 0 || pageSize <= 0 || timeline.totalCount() <= 0) {
        return 0;
    }
    const qint64 first = static_cast<qint64>(timeline.totalCount())
        - static_cast<qint64>(page) * ChannelPageSize - pageSize;
    return static_cast<int>(std::max<qint64>(0, first));
}

ChannelTimelineController::ViewportAnchor ChannelTimelineController::stableViewportAnchor() const
{
    if (lastUserViewportAnchor.isValid()) {
        return lastUserViewportAnchor;
    }
    return captureViewportAnchor();
}

void ChannelTimelineController::requestPage(int page, int focusLogicalIndex)
{
    Q_UNUSED(focusLogicalIndex);

    if (!active || requestInFlight || page < 0) {
        return;
    }

    if (totalCountExact && expectedPostCount >= 0
        && static_cast<qint64>(page) * ChannelPageSize >= expectedPostCount
        && !(page == 0 && expectedPostCount == 0)) {
        return;
    }

    if (loadedPages.contains(page)) {
        continueInitialPrefetch();
        return;
    }

    requestInFlight = true;
    requestedPage = page;
    setLoadingIndicator(area.ui ? area.ui->loadOldPosts : nullptr, true);
    const quint64 requestGeneration = generation;
    QPointer<ChannelTimelineController> guard(this);

    PostTimelineService::instance(area.backend).loadChannelPage(
        area.channel, page, ChannelPageSize,
        [guard, requestGeneration, page](const PostTimelineService::Page& result) {
            if (!guard || !guard->active || guard->generation != requestGeneration) {
                return;
            }

            setLoadingIndicator(guard->area.ui ? guard->area.ui->loadOldPosts : nullptr, false);
            guard->requestInFlight = false;
            guard->requestedPage = -1;

            if (!result.success) {
                guard->flushDeferredExternalPosts();
                return;
            }

            const ViewportAnchor anchor = guard->stableViewportAnchor();
            const QStringList ids = result.postIds;
            const int responseSize = static_cast<int>(ids.size());
            const int minimumKnownCount = page * ChannelPageSize + responseSize;
            const bool reachedOldestEdge = result.prevPostId.isEmpty();

            int discoveredCount = guard->expectedPostCount;
            const int reportedRootCount = guard->channelRootPostCount();
            if (reachedOldestEdge) {
                discoveredCount = minimumKnownCount;
                guard->totalCountExact = true;
                guard->initialPageTarget = std::min(guard->initialPageTarget, page + 1);
            } else if (reportedRootCount > 0) {
                discoveredCount = std::max(reportedRootCount, minimumKnownCount);
                guard->totalCountExact = true;
            } else if (!guard->totalCountExact) {
                discoveredCount = std::max(
                    guard->expectedPostCount,
                    minimumKnownCount + ChannelPageSize);
            } else {
                discoveredCount = std::max(guard->expectedPostCount, minimumKnownCount);
            }

            discoveredCount = std::max(0, discoveredCount);
            if (discoveredCount != guard->timeline.totalCount()) {
                guard->timeline.setTotalCountPreservingNewest(discoveredCount);
            }
            guard->expectedPostCount = discoveredCount;

            guard->loadedPages.insert(page);
            while (guard->loadedPages.contains(guard->nextOlderPage)) {
                ++guard->nextOlderPage;
            }

            if (!ids.isEmpty()) {
                const int firstIndex = guard->authoritativeFirstIndex(page, responseSize);
                guard->timeline.placeWindow(firstIndex, ids);
                if (reachedOldestEdge) {
                    guard->timeline.alignLoadedSpanToBoundary(ids.first(), true);
                }
            }

            if (!guard->initialRenderDone) {
                guard->continueInitialPrefetch();
                return;
            }

            guard->renderTimeline(QString(), anchor);
            guard->flushDeferredExternalPosts();
            guard->scheduleViewportCheck();
        });
}

void ChannelTimelineController::requestContextBefore()
{
    if (!active || requestInFlight || !contextNavigationActive
        || contextReachedOldest || contextOldestPostId.isEmpty()) {
        return;
    }

    const QString cursorId = contextOldestPostId;
    if (timeline.indexOf(cursorId) < 0) {
        return;
    }

    requestInFlight = true;
    setLoadingIndicator(area.ui ? area.ui->loadOldPosts : nullptr, true);
    const quint64 requestGeneration = generation;
    QPointer<ChannelTimelineController> guard(this);
    PostTimelineService::instance(area.backend).loadChannelBefore(
        area.channel, cursorId, ChannelPageSize,
        [guard, requestGeneration, cursorId](const PostTimelineService::Page& result) {
            if (!guard || !guard->active || guard->generation != requestGeneration) {
                return;
            }
            setLoadingIndicator(guard->area.ui ? guard->area.ui->loadOldPosts : nullptr, false);
            guard->requestInFlight = false;
            if (!result.success || !guard->contextNavigationActive) {
                guard->flushDeferredExternalPosts();
                return;
            }

            const ViewportAnchor anchor = guard->stableViewportAnchor();
            QStringList ids;
            for (const QString& id : uniqueChronologicalRootIds(guard->area.channel, result.postIds)) {
                if (!guard->timeline.contains(id)) {
                    ids.push_back(id);
                }
            }

            int cursorIndex = guard->timeline.indexOf(cursorId);
            if (cursorIndex >= 0 && !ids.isEmpty()) {
                int firstIndex = cursorIndex - static_cast<int>(ids.size());
                if (firstIndex < 0) {
                    const int deficit = -firstIndex;
                    if (!guard->totalCountExact) {
                        const int grown = guard->timeline.totalCount() + deficit;
                        guard->timeline.setTotalCountPreservingNewest(grown);
                        guard->expectedPostCount = grown;
                        cursorIndex += deficit;
                        firstIndex = 0;
                    } else {
                        ids = ids.mid(deficit);
                        firstIndex = 0;
                    }
                }
                if (!ids.isEmpty()) {
                    guard->timeline.placeWindow(firstIndex, ids);
                    guard->contextOldestPostId = ids.first();
                }
            }

            if (result.prevPostId.isEmpty()) {
                guard->contextReachedOldest = true;
            }
            if (guard->contextReachedOldest && !guard->contextOldestPostId.isEmpty()) {
                guard->timeline.alignLoadedSpanToBoundary(guard->contextOldestPostId, true);
            }
            guard->renderTimeline(QString(), anchor);
            guard->flushDeferredExternalPosts();
            guard->scheduleViewportCheck();
        });
}

void ChannelTimelineController::requestContextAfter()
{
    if (!active || requestInFlight || !contextNavigationActive
        || contextReachedNewest || contextNewestPostId.isEmpty()) {
        return;
    }

    const QString cursorId = contextNewestPostId;
    if (timeline.indexOf(cursorId) < 0) {
        return;
    }

    requestInFlight = true;
    setLoadingIndicator(area.ui ? area.ui->loadOldPosts : nullptr, true);
    const quint64 requestGeneration = generation;
    QPointer<ChannelTimelineController> guard(this);
    PostTimelineService::instance(area.backend).loadChannelAfter(
        area.channel, cursorId, ChannelPageSize,
        [guard, requestGeneration, cursorId](const PostTimelineService::Page& result) {
            if (!guard || !guard->active || guard->generation != requestGeneration) {
                return;
            }
            setLoadingIndicator(guard->area.ui ? guard->area.ui->loadOldPosts : nullptr, false);
            guard->requestInFlight = false;
            if (!result.success || !guard->contextNavigationActive) {
                guard->flushDeferredExternalPosts();
                return;
            }

            const ViewportAnchor anchor = guard->stableViewportAnchor();
            QStringList ids;
            for (const QString& id : uniqueChronologicalRootIds(guard->area.channel, result.postIds)) {
                if (!guard->timeline.contains(id)) {
                    ids.push_back(id);
                }
            }

            const int cursorIndex = guard->timeline.indexOf(cursorId);
            if (cursorIndex >= 0 && !ids.isEmpty()) {
                const int firstIndex = cursorIndex + 1;
                const int neededTotal = firstIndex + static_cast<int>(ids.size());
                if (neededTotal > guard->timeline.totalCount() && !guard->totalCountExact) {
                    guard->timeline.setTotalCount(neededTotal);
                    guard->expectedPostCount = neededTotal;
                }
                const int available = std::max(0, guard->timeline.totalCount() - firstIndex);
                if (static_cast<int>(ids.size()) > available) {
                    ids = ids.mid(0, available);
                }
                if (!ids.isEmpty()) {
                    guard->timeline.placeWindow(firstIndex, ids);
                    guard->contextNewestPostId = ids.last();
                }
            }

            if (result.nextPostId.isEmpty()) {
                guard->contextReachedNewest = true;
            }
            if (guard->contextReachedNewest && !guard->contextNewestPostId.isEmpty()) {
                guard->timeline.alignLoadedSpanToBoundary(guard->contextNewestPostId, false);
            }
            guard->renderTimeline(QString(), anchor);
            guard->flushDeferredExternalPosts();
            guard->scheduleViewportCheck();
        });
}

void ChannelTimelineController::continueInitialPrefetch()
{
    if (!active || requestInFlight) {
        return;
    }

    for (int page = 0; page < initialPageTarget; ++page) {
        if (!loadedPages.contains(page)) {
            requestPage(page);
            return;
        }
    }

    if (!initialRenderDone) {
        flushDeferredExternalPosts(false);
        initialRenderDone = true;
        renderTimeline();
    }
    scheduleViewportCheck();
}

void ChannelTimelineController::requestOlderPage()
{
    if (!active || requestInFlight || !area.ui || !area.ui->listWidget
        || area.ui->listWidget->hasTimelineNavigationLock()) {
        return;
    }
    if (contextNavigationActive && !contextOldestPostId.isEmpty()) {
        requestContextBefore();
        return;
    }
    if (totalCountExact
        && static_cast<qint64>(nextOlderPage) * ChannelPageSize >= expectedPostCount) {
        return;
    }
    requestPage(nextOlderPage);
}

void ChannelTimelineController::requestPageForIndex(int logicalIndex, bool focusAfterLoad)
{
    Q_UNUSED(focusAfterLoad);

    if (!active || logicalIndex < 0 || logicalIndex >= timeline.totalCount()) {
        return;
    }

    if (!timeline.postIdAt(logicalIndex).isEmpty()) {
        return;
    }

    const int distanceFromNewest = timeline.totalCount() - 1 - logicalIndex;
    const int page = std::max(0, distanceFromNewest / ChannelPageSize);
    requestPage(page);
}

void ChannelTimelineController::requestSeek(int logicalIndex)
{
    pendingSeekIndex = -1;
    requestPageForIndex(logicalIndex, false);
}

void ChannelTimelineController::absorbNewPosts(const ChannelNewPosts& newPosts)
{
    if (!active) {
        return;
    }

    QStringList ids;
    for (const ChannelNewPostsChunk& chunk : newPosts.postsToAdd) {
        for (BackendPost* post : chunk.postsToAdd) {
            if (!post || post->hidden || !post->root_id.isEmpty()) {
                continue;
            }
            ids.push_back(post->id);
        }
    }

    ids = uniqueChronologicalRootIds(area.channel, ids);
    if (ids.isEmpty()) {
        return;
    }

    if (!initialRenderDone || requestInFlight) {
        deferredExternalPostIds.append(ids);
        return;
    }

    placeApproximateWindow(ids);
}

void ChannelTimelineController::flushDeferredExternalPosts(bool renderAfter)
{
    if (!active || requestInFlight || deferredExternalPostIds.isEmpty()) {
        return;
    }

    const QStringList ids = deferredExternalPostIds;
    deferredExternalPostIds.clear();
    placeApproximateWindow(ids, QString(), renderAfter);
}

int ChannelTimelineController::estimateLogicalIndex(uint64_t createAt) const
{
    const int total = timeline.totalCount();
    if (total <= 1) {
        return 0;
    }

    uint64_t start = area.channel.create_at;
    uint64_t end = area.channel.last_post_at;
    if (end <= start) {
        end = start + 1;
    }

    if (createAt <= start) {
        return 0;
    }
    if (createAt >= end) {
        return total - 1;
    }

    const long double fraction = static_cast<long double>(createAt - start)
        / static_cast<long double>(end - start);
    const long double index = fraction * static_cast<long double>(total - 1);
    return std::max(0, std::min(total - 1, static_cast<int>(std::llround(index))));
}

int ChannelTimelineController::gapPlacementForWindow(int estimatedCenter, int count) const
{
    if (count <= 0) {
        return -1;
    }

    int bestFirst = -1;
    qint64 bestDistance = LLONG_MAX;
    for (const PostTimeline::Span& span : timeline.spans()) {
        if (span.kind != PostTimeline::GapSpan || span.count < count) {
            continue;
        }

        const int maxFirst = span.firstIndex + span.count - count;
        const int idealFirst = estimatedCenter - count / 2;
        const int first = std::max(span.firstIndex, std::min(maxFirst, idealFirst));
        const int placedCenter = first + count / 2;
        const qint64 distance = std::llabs(static_cast<long long>(placedCenter)
                                          - static_cast<long long>(estimatedCenter));
        if (distance < bestDistance) {
            bestDistance = distance;
            bestFirst = first;
        }
    }
    return bestFirst;
}

void ChannelTimelineController::placeApproximateWindow(const QStringList& postIds,
                                                        const QString& focusPostId,
                                                        bool renderAfter)
{
    if (!active || postIds.isEmpty()) {
        return;
    }

    QStringList missing;
    for (const QString& id : uniqueChronologicalRootIds(area.channel, postIds)) {
        if (!timeline.contains(id)) {
            missing.push_back(id);
        }
    }

    if (missing.isEmpty()) {
        if (renderAfter && !focusPostId.isEmpty()) {
            renderTimeline(focusPostId, stableViewportAnchor());
        }
        return;
    }

    const ViewportAnchor anchor = stableViewportAnchor();

    if (timeline.totalCount() <= 0) {
        expectedPostCount = std::max(channelRootPostCount(), static_cast<int>(missing.size()));
        timeline.setTotalCount(expectedPostCount);
    }

    BackendPost* middle = area.channel.postIdToPost.value(
        missing.at(static_cast<int>(missing.size()) / 2), nullptr);
    const int estimatedCenter = middle
        ? estimateLogicalIndex(middle->create_at)
        : timeline.totalCount() / 2;
    const int first = gapPlacementForWindow(estimatedCenter,
                                            static_cast<int>(missing.size()));
    if (first < 0) {
        return;
    }

    timeline.placeWindow(first, missing);
    if (renderAfter) {
        renderTimeline(focusPostId, anchor);
    }
}

bool ChannelTimelineController::ensurePostVisible(const QString& postId)
{
    if (!active || postId.isEmpty() || !area.ui || !area.ui->listWidget) {
        return false;
    }

    if (area.ui->listWidget->findPost(postId)) {
        return true;
    }

    BackendPost* target = area.channel.postIdToPost.value(postId, nullptr);
    if (!target || target->hidden || !target->root_id.isEmpty()) {
        return false;
    }

    if (timeline.contains(postId)) {
        renderTimeline(postId, stableViewportAnchor());
        return area.ui->listWidget->findPost(postId) != nullptr;
    }

    QVector<BackendPost*> roots;
    roots.reserve(static_cast<int>(area.channel.posts.size()));
    int targetIndex = -1;
    for (BackendPost& post : area.channel.posts) {
        if (post.hidden || !post.root_id.isEmpty()) {
            continue;
        }
        if (post.id == postId) {
            targetIndex = roots.size();
        }
        roots.push_back(&post);
    }

    if (targetIndex < 0) {
        return false;
    }

    const int firstCached = std::max(0, targetIndex - ContextPostsPerSide);
    const int lastCached = std::min(static_cast<int>(roots.size()),
                                    targetIndex + ContextPostsPerSide + 1);
    QStringList contextIds;
    for (int i = firstCached; i < lastCached; ++i) {
        contextIds.push_back(roots.at(i)->id);
    }

    if (contextNavigationActive && contextNavigationPostId == postId && !contextIds.isEmpty()) {
        contextOldestPostId = contextIds.first();
        contextNewestPostId = contextIds.last();
    }

    placeApproximateWindow(contextIds, postId);
    return area.ui->listWidget->findPost(postId) != nullptr;
}

ChannelTimelineController::ViewportAnchor ChannelTimelineController::captureViewportAnchor() const
{
    ViewportAnchor anchor;
    if (!active || !area.ui || !area.ui->listWidget) {
        return anchor;
    }

    PostsListWidget* list = area.ui->listWidget;
    if (list->count() == 0 || list->viewport()->height() <= 0) {
        return anchor;
    }

    if (list->isAtBottom()) {
        anchor.kind = ViewportAnchor::Bottom;
        return anchor;
    }

    const QRect viewportRect = list->viewport()->rect();
    const int centerY = viewportRect.center().y();
    int bestDistance = INT_MAX;
    for (int row = 0; row < list->count(); ++row) {
        QListWidgetItem* item = list->item(row);
        if (!PostsListWidget::isPostItem(item)) {
            continue;
        }
        const QRect rect = list->visualItemRect(item);
        if (!rect.isValid() || !rect.intersects(viewportRect)) {
            continue;
        }
        const int distance = std::abs(rect.center().y() - centerY);
        if (distance >= bestDistance) {
            continue;
        }
        const QString postId = item->data(ItemRole::postId).toString();
        if (postId.isEmpty()) {
            continue;
        }
        bestDistance = distance;
        anchor.kind = ViewportAnchor::Post;
        anchor.postId = postId;
        anchor.postTopOffset = rect.top();
    }

    if (anchor.kind == ViewportAnchor::Post) {
        return anchor;
    }

    const qint64 centerPixel = static_cast<qint64>(list->verticalScrollBar()->value())
        + list->viewport()->height() / 2;
    const PostTimeline::PixelLocation location = timeline.locatePixel(centerPixel);
    if (!location.isValid()) {
        return anchor;
    }

    anchor.kind = ViewportAnchor::Gap;
    anchor.distanceFromNewest = timeline.totalCount() - 1 - location.logicalIndex;
    anchor.offsetWithinEstimatedRow = location.offsetWithinRow;
    return anchor;
}

void ChannelTimelineController::restoreViewportAnchor(const ViewportAnchor& anchor,
                                                      const QString& focusPostId)
{
    if (!area.ui || !area.ui->listWidget) {
        return;
    }

    PostsListWidget* list = area.ui->listWidget;

    if (!focusPostId.isEmpty()) {
        const int row = list->findPostByIndex(focusPostId, 0);
        if (row >= 0) {
            const int topOffset = std::max(0, list->viewport()->height() / 3);
            list->finishTimelineRebuildAtPost(focusPostId, topOffset);
            list->highlightPost(focusPostId);
            return;
        }
    }

    if (anchor.kind == ViewportAnchor::Bottom || !anchor.isValid()) {
        list->finishTimelineRebuildAtBottom();
        return;
    }

    if (anchor.kind == ViewportAnchor::Post
        && list->finishTimelineRebuildAtPost(anchor.postId, anchor.postTopOffset)) {
        return;
    }

    if (anchor.kind == ViewportAnchor::Gap && timeline.totalCount() > 0) {
        const int index = std::max(0,
            std::min(timeline.totalCount() - 1,
                     timeline.totalCount() - 1 - anchor.distanceFromNewest));
        const qint64 centerPixel = timeline.estimatedPixelForIndex(index)
            + anchor.offsetWithinEstimatedRow;
        const qint64 viewportTop = centerPixel - list->viewport()->height() / 2;
        list->finishTimelineRebuildAtPixel(viewportTop);
        return;
    }

    list->finishTimelineRebuildAtBottom();
}

void ChannelTimelineController::scheduleViewportCheck()
{
    if (!active || viewportCheckScheduled || rebuilding) {
        return;
    }
    viewportCheckScheduled = true;
    QTimer::singleShot(0, this, [this] {
        viewportCheckScheduled = false;
        checkViewport();
    });
}

int ChannelTimelineController::logicalIndexNearViewport(int prefetchRows,
                                                        bool* centerInsideGap) const
{
    if (centerInsideGap) {
        *centerInsideGap = false;
    }
    if (!active || !area.ui || !area.ui->listWidget || timeline.totalCount() <= 0) {
        return -1;
    }

    PostsListWidget* list = area.ui->listWidget;
    QScrollBar* bar = list->verticalScrollBar();
    const int viewportHeight = list->viewport()->height();
    if (viewportHeight <= 0) {
        return -1;
    }

    const qint64 centerPixel = static_cast<qint64>(bar->value()) + viewportHeight / 2;
    const PostTimeline::PixelLocation center = timeline.locatePixel(centerPixel);
    if (centerInsideGap) {
        *centerInsideGap = center.isValid() && !center.loaded;
    }
    if (center.isValid() && !center.loaded) {
        return center.logicalIndex;
    }

    const QRect viewportRect = list->viewport()->rect();
    int firstVisibleIndex = INT_MAX;
    int lastVisibleIndex = -1;
    for (int row = 0; row < list->count(); ++row) {
        QListWidgetItem* item = list->item(row);
        if (!PostsListWidget::isPostItem(item)) {
            continue;
        }
        const QRect rect = list->visualItemRect(item);
        if (!rect.isValid() || !rect.intersects(viewportRect)) {
            continue;
        }
        const QString postId = item->data(ItemRole::postId).toString();
        const int logicalIndex = timeline.indexOf(postId);
        if (logicalIndex < 0) {
            continue;
        }
        firstVisibleIndex = std::min(firstVisibleIndex, logicalIndex);
        lastVisibleIndex = std::max(lastVisibleIndex, logicalIndex);
    }

    if (lastVisibleIndex < 0) {
        return -1;
    }

    const int threshold = std::max(0, prefetchRows);
    const int olderGap = timeline.adjacentGapIndex(firstVisibleIndex, true, threshold);
    const int newerGap = timeline.adjacentGapIndex(lastVisibleIndex, false, threshold);
    if (olderGap >= 0 && newerGap >= 0) {
        const int olderDistance = firstVisibleIndex - olderGap;
        const int newerDistance = newerGap - lastVisibleIndex;
        return olderDistance <= newerDistance ? olderGap : newerGap;
    }
    return olderGap >= 0 ? olderGap : newerGap;
}

void ChannelTimelineController::checkViewport()
{
    if (!active || requestInFlight || !area.ui || !area.ui->listWidget) {
        return;
    }

    PostsListWidget* list = area.ui->listWidget;
    if (list->hasTimelineNavigationLock()) {
        pendingSeekIndex = -1;
        seekTimer.stop();
        return;
    }

    QScrollBar* scrollBar = list->verticalScrollBar();

    bool centerInsideGap = false;
    const int targetIndex = logicalIndexNearViewport(GapPrefetchRows, &centerInsideGap);
    if (targetIndex < 0) {
        pendingSeekIndex = -1;
        seekTimer.stop();
        return;
    }

    if (contextNavigationActive
        && !contextOldestPostId.isEmpty()
        && !contextNewestPostId.isEmpty()) {
        const int oldestIndex = timeline.indexOf(contextOldestPostId);
        const int newestIndex = timeline.indexOf(contextNewestPostId);
        if (oldestIndex >= 0 && targetIndex < oldestIndex) {
            requestContextBefore();
            return;
        }
        if (newestIndex >= 0 && targetIndex > newestIndex) {
            requestContextAfter();
            return;
        }
        pendingSeekIndex = -1;
        seekTimer.stop();
        return;
    }

    if (scrollBar->isSliderDown() && centerInsideGap) {
        pendingSeekIndex = targetIndex;
        seekTimer.start();
        return;
    }

    pendingSeekIndex = -1;
    seekTimer.stop();
    requestPageForIndex(targetIndex, false);
}

void ChannelTimelineController::renderTimeline(const QString& focusPostId,
                                               const ViewportAnchor& requestedAnchor)
{
    if (!active || !area.ui || !area.ui->listWidget || rebuilding) {
        return;
    }

    ViewportAnchor anchor = requestedAnchor;
    if (!anchor.isValid() && initialRenderDone) {
        anchor = stableViewportAnchor();
    }

    rebuilding = true;
    const quint64 renderId = ++renderGeneration;
    PostsListWidget* list = area.ui->listWidget;
    list->setUpdatesEnabled(false);
    list->beginTimelineRebuild();

    const QDate currentDate = QDateTime::currentDateTime().date();
    for (const PostTimeline::Span& span : timeline.spans()) {
        if (span.kind == PostTimeline::GapSpan) {
            if (span.count <= 0 || span.estimatedHeight <= 0) {
                continue;
            }

            auto* gapItem = new QListWidgetItem;
            gapItem->setData(Qt::UserRole, ItemType::gap);
            gapItem->setData(ItemRole::gapFirstIndex, span.firstIndex);
            gapItem->setData(ItemRole::gapCount, span.count);
            gapItem->setFlags(Qt::NoItemFlags);
            const qint64 boundedHeight = std::min<qint64>(span.estimatedHeight, INT_MAX);
            gapItem->setSizeHint(QSize(0, static_cast<int>(boundedHeight)));
            list->addItem(gapItem);
            continue;
        }

        int elapsedDays = INT_MAX;
        for (const QString& postId : span.postIds) {
            BackendPost* post = area.channel.postIdToPost.value(postId, nullptr);
            if (!post || post->hidden || !post->root_id.isEmpty()) {
                continue;
            }

            const int postDays = post->getCreationTime().date().daysTo(currentDate);
            if (postDays != elapsedDays) {
                elapsedDays = postDays;
                list->addDaySeparator(list->count(), postDays);
            }

            auto* postWidget = new PostWidget(area.backend, *post, list, &area, nullptr);
            list->insertPost(postWidget);
            connect(postWidget, &PostWidget::dimensionsChanged,
                    this, &ChannelTimelineController::scheduleMeasurementPass);
        }
    }

    area.areaIsFilled = timeline.loadedCount() > 20;
    area.gettingOlderPosts = true;
    rebuilding = false;

    restoreViewportAnchor(anchor, focusPostId);
    scheduleMeasurementPass();
    schedulePrune();
    schedulePaintResume(renderId);
    QTimer::singleShot(0, this, &ChannelTimelineController::scheduleViewportCheck);
}

void ChannelTimelineController::schedulePaintResume(quint64 renderId)
{
    QPointer<PostsListWidget> list(area.ui ? area.ui->listWidget : nullptr);
    QTimer::singleShot(0, this, [this, list, renderId] {
        QTimer::singleShot(0, this, [this, list, renderId] {
            if (!list || renderGeneration != renderId) {
                return;
            }
            list->setUpdatesEnabled(true);
            list->viewport()->update();
        });
    });
}

void ChannelTimelineController::updateGapHeights()
{
    if (!active || !area.ui || !area.ui->listWidget) {
        return;
    }

    PostsListWidget* list = area.ui->listWidget;
    const ViewportAnchor anchor = stableViewportAnchor();
    if (anchor.kind == ViewportAnchor::Gap) {
        return;
    }

    const int average = timeline.estimatedRowHeight();
    bool changed = false;
    const quint64 renderId = ++renderGeneration;
    list->setUpdatesEnabled(false);
    for (int row = 0; row < list->count(); ++row) {
        QListWidgetItem* item = list->item(row);
        if (!PostsListWidget::isGapItem(item)) {
            continue;
        }

        const int count = item->data(ItemRole::gapCount).toInt();
        const qint64 height = static_cast<qint64>(std::max(0, count)) * average;
        const int bounded = static_cast<int>(std::min<qint64>(height, INT_MAX));
        if (item->sizeHint().height() != bounded) {
            item->setSizeHint(QSize(0, bounded));
            changed = true;
        }
    }

    if (changed) {
        restoreViewportAnchor(anchor);
    }
    lastAppliedGapRowHeight = average;
    schedulePaintResume(renderId);
}

void ChannelTimelineController::scheduleMeasurementPass()
{
    if (!active || rebuilding) {
        return;
    }
    measurementTimer.start();
}

void ChannelTimelineController::measureRenderedPosts()
{
    if (!active || !area.ui || !area.ui->listWidget || rebuilding) {
        return;
    }

    PostsListWidget* list = area.ui->listWidget;
    for (int row = 0; row < list->count(); ++row) {
        QListWidgetItem* item = list->item(row);
        if (!PostsListWidget::isPostItem(item)) {
            continue;
        }
        const QString postId = item->data(ItemRole::postId).toString();
        const int height = item->sizeHint().height();
        if (!postId.isEmpty() && height > 0) {
            timeline.recordMeasuredHeight(postId, height);
        }
    }

    const int average = timeline.estimatedRowHeight();
    const int baseline = std::max(1, lastAppliedGapRowHeight);
    const int difference = std::abs(average - lastAppliedGapRowHeight);
    if (difference * 5 >= baseline) {
        updateGapHeights();
    }
}

} // namespace Mattermost
