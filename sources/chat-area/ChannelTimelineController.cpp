#include "ChannelTimelineController.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>

#include <QAbstractItemView>
#include <QDateTime>
#include <QEvent>
#include <QListWidgetItem>
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

constexpr int ChannelPageSize = 80;
constexpr int SmallChannelPrefetchPages = 2;
constexpr int LargeChannelPrefetchPages = 3;
constexpr int SeekDebounceMs = 140;
constexpr int MeasurementDebounceMs = 120;
constexpr int GapPrefetchScreens = 3;
constexpr int ContextPostsPerSide = 30;

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
        // QStackedWidget can emit transient Hide events while the initial page
        // hierarchy is being assembled. Deactivate only after the backend has
        // actually selected another channel; otherwise the very first selected
        // chat can lose its controller before the first page arrives.
        QPointer<ChannelTimelineController> guard(this);
        QTimer::singleShot(0, this, [guard] {
            if (!guard || !guard->active) {
                return;
            }
            if (guard->area.backend.getCurrentChannel() != &guard->area.channel) {
                guard->deactivate();
                return;
            }
            if (!guard->area.isVisible()) {
                QTimer::singleShot(50, guard, [guard] {
                    if (guard && guard->active && !guard->area.isVisible()
                        && guard->area.backend.getCurrentChannel() != &guard->area.channel) {
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

    if (!area.isVisible() && area.backend.getCurrentChannel() != &area.channel) {
        return;
    }
    start();
}

int ChannelTimelineController::channelRootPostCount() const
{
    // total_msg_count includes replies on servers that do not expose the root
    // counter. Using it as an exact root count manufactures a phantom gap for
    // every thread reply. Unknown is safer: page responses grow a provisional
    // oldest-side gap until the server marks the real boundary.
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
    requestInFlight = false;
    requestedPage = -1;
    requestedFocusIndex = -1;
    pendingSeekIndex = -1;
    viewportCheckScheduled = false;
    deferredExternalFlushScheduled = false;
    loadedPages.clear();
    deferredExternalPostIds.clear();
    nextOlderPage = 0;
    initialRenderDone = false;

    totalCountExact = area.channel.has_total_msg_count_root;
    expectedPostCount = channelRootPostCount();
    timeline.reset(expectedPostCount);
    lastAppliedGapRowHeight = timeline.estimatedRowHeight();

    QObject::disconnect(&area.channel, &BackendChannel::onNewPosts,
                        &area, &ChatArea::fillChannelPosts);

    area.gettingOlderPosts = true;

    connect(area.ui->loadOldPosts, &QPushButton::clicked,
            this, &ChannelTimelineController::requestOlderPage);
    connect(area.ui->listWidget, &PostsListWidget::scrolledToTop,
            this, &ChannelTimelineController::requestOlderPage);

    QScrollBar* scrollBar = area.ui->listWidget->verticalScrollBar();
    connect(scrollBar, &QScrollBar::valueChanged, this,
            [this](int) { scheduleViewportCheck(); });
    connect(scrollBar, &QScrollBar::sliderReleased, this,
            [this] { scheduleViewportCheck(); });

    connect(&area.channel, &BackendChannel::onNewPosts, this,
            &ChannelTimelineController::absorbNewPosts);

    connect(&area.channel, &BackendChannel::onNewPost, this,
            [this](BackendPost& post) {
        if (!active || post.hidden || !post.root_id.isEmpty()) {
            return;
        }

        if (expectedPostCount < INT_MAX) {
            ++expectedPostCount;
        }
        timeline.setTotalCount(expectedPostCount);
        timeline.placeWindow(expectedPostCount - 1, QStringList {post.id});
        scheduleMeasurementPass();
    });

    if (totalCountExact) {
        const int pages = std::max(1,
            (expectedPostCount + ChannelPageSize - 1) / ChannelPageSize);
        initialPageTarget = std::min(
            pages,
            expectedPostCount > ChannelPageSize * SmallChannelPrefetchPages
                ? LargeChannelPrefetchPages
                : SmallChannelPrefetchPages);
    } else {
        initialPageTarget = SmallChannelPrefetchPages;
    }
    initialPageTarget = std::max(1, initialPageTarget);

    // Do not rebuild after every prefetched page. The previous implementation
    // destroyed/recreated every PostWidget 2-3 times during channel opening,
    // retriggering image layout and making the viewport visibly oscillate.
    continueInitialPrefetch();
}

void ChannelTimelineController::deactivate()
{
    if (!active) {
        return;
    }

    active = false;
    ++generation;
    ++renderGeneration;
    seekTimer.stop();
    measurementTimer.stop();
    pendingSeekIndex = -1;
    requestInFlight = false;
    area.gettingOlderPosts = false;

    QObject::disconnect(&area.channel, nullptr, this, nullptr);
    if (area.ui) {
        if (area.ui->loadOldPosts) {
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

void ChannelTimelineController::requestPage(int page, int focusLogicalIndex)
{
    if (!active || requestInFlight || page < 0) {
        return;
    }

    if (totalCountExact && expectedPostCount >= 0
        && static_cast<qint64>(page) * ChannelPageSize >= expectedPostCount
        && !(page == 0 && expectedPostCount == 0)) {
        return;
    }

    if (loadedPages.contains(page)) {
        if (focusLogicalIndex >= 0) {
            const QString focusId = timeline.postIdAt(focusLogicalIndex);
            if (!focusId.isEmpty()) {
                renderTimeline(focusId);
            }
        }
        continueInitialPrefetch();
        return;
    }

    requestInFlight = true;
    requestedPage = page;
    requestedFocusIndex = focusLogicalIndex;
    const quint64 requestGeneration = generation;
    QPointer<ChannelTimelineController> guard(this);

    PostTimelineService::instance(area.backend).loadChannelPage(
        area.channel, page, ChannelPageSize,
        [guard, requestGeneration, page](const PostTimelineService::Page& result) {
            if (!guard || !guard->active || guard->generation != requestGeneration) {
                return;
            }

            const int focusIndex = guard->requestedFocusIndex;
            guard->requestInFlight = false;
            guard->requestedPage = -1;
            guard->requestedFocusIndex = -1;

            if (!result.success) {
                guard->flushDeferredExternalPosts();
                return;
            }

            const QStringList ids = result.postIds;
            const int responseSize = static_cast<int>(ids.size());
            const int minimumKnownCount = page * ChannelPageSize + responseSize;
            // Mattermost itself uses an empty prev_post_id as the authoritative
            // oldest-edge marker. Page length is not reliable here: filtering
            // collapsed/thread replies can make a non-final root page short.
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
                // Keep exactly one not-yet-proven older page as provisional
                // geometry. If another page exists, the whole loaded tail is
                // shifted right before that page is placed.
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

            QString focusId;
            if (!ids.isEmpty()) {
                const int firstIndex = guard->authoritativeFirstIndex(page, responseSize);
                guard->timeline.placeWindow(firstIndex, ids);
                if (focusIndex >= 0) {
                    const int clamped = std::max(firstIndex,
                        std::min(focusIndex, firstIndex + responseSize - 1));
                    focusId = guard->timeline.postIdAt(clamped);
                }
            }

            if (!guard->initialRenderDone) {
                guard->continueInitialPrefetch();
                return;
            }

            guard->renderTimeline(focusId);
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
    if (!active || requestInFlight) {
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
    if (!active || logicalIndex < 0 || logicalIndex >= timeline.totalCount()) {
        return;
    }

    const QString loadedId = timeline.postIdAt(logicalIndex);
    if (!loadedId.isEmpty()) {
        if (focusAfterLoad) {
            renderTimeline(loadedId);
        }
        return;
    }

    const int distanceFromNewest = timeline.totalCount() - 1 - logicalIndex;
    const int page = std::max(0, distanceFromNewest / ChannelPageSize);
    requestPage(page, focusAfterLoad ? logicalIndex : -1);
}

void ChannelTimelineController::requestSeek(int logicalIndex)
{
    pendingSeekIndex = -1;
    requestPageForIndex(logicalIndex, true);
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
            renderTimeline(focusPostId);
        }
        return;
    }

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
        renderTimeline(focusPostId);
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
        renderTimeline(postId);
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

    placeApproximateWindow(contextIds, postId);
    return area.ui->listWidget->findPost(postId) != nullptr;
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

int ChannelTimelineController::logicalIndexForNearbyGap(bool* viewportCenterInsideGap) const
{
    if (viewportCenterInsideGap) {
        *viewportCenterInsideGap = false;
    }
    if (!active || !area.ui || !area.ui->listWidget) {
        return -1;
    }

    PostsListWidget* list = area.ui->listWidget;
    const int viewportHeight = list->viewport()->height();
    if (viewportHeight <= 0) {
        return -1;
    }

    const int centerY = viewportHeight / 2;
    const int margin = viewportHeight * GapPrefetchScreens;
    const int average = std::max(1, timeline.estimatedRowHeight());
    int bestIndex = -1;
    int bestDistance = INT_MAX;
    bool bestContainsCenter = false;

    for (int row = 0; row < list->count(); ++row) {
        QListWidgetItem* item = list->item(row);
        if (!PostsListWidget::isGapItem(item)) {
            continue;
        }

        const QRect rect = list->visualItemRect(item);
        if (!rect.isValid() || rect.height() <= 0) {
            continue;
        }
        if (rect.bottom() < -margin || rect.top() > viewportHeight + margin) {
            continue;
        }

        const int first = item->data(ItemRole::gapFirstIndex).toInt();
        const int count = item->data(ItemRole::gapCount).toInt();
        if (count <= 0) {
            continue;
        }

        const bool containsCenter = rect.top() <= centerY && rect.bottom() >= centerY;
        const int yInside = std::max(0, std::min(rect.height() - 1, centerY - rect.top()));
        const int offset = std::min(count - 1, yInside / average);
        const int candidate = first + offset;

        int distance = 0;
        if (!containsCenter) {
            distance = rect.bottom() < centerY
                ? centerY - rect.bottom()
                : rect.top() - centerY;
        }

        if (distance < bestDistance) {
            bestDistance = distance;
            bestIndex = candidate;
            bestContainsCenter = containsCenter;
        }
    }

    if (viewportCenterInsideGap) {
        *viewportCenterInsideGap = bestContainsCenter;
    }
    return bestIndex;
}

void ChannelTimelineController::checkViewport()
{
    if (!active || requestInFlight || !area.ui || !area.ui->listWidget) {
        return;
    }

    PostsListWidget* list = area.ui->listWidget;
    QScrollBar* scrollBar = list->verticalScrollBar();

    bool centerInsideGap = false;
    const int targetIndex = logicalIndexForNearbyGap(&centerInsideGap);
    if (targetIndex < 0) {
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
    requestPageForIndex(targetIndex, centerInsideGap);
}

void ChannelTimelineController::renderTimeline(const QString& focusPostId)
{
    if (!active || !area.ui || !area.ui->listWidget || rebuilding) {
        return;
    }

    rebuilding = true;
    const quint64 renderId = ++renderGeneration;
    PostsListWidget* list = area.ui->listWidget;
    list->commitCurrentViewportAsAnchor();
    list->setUpdatesEnabled(false);
    list->clear();

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

    bool focused = false;
    if (!focusPostId.isEmpty()) {
        const int row = list->findPostByIndex(focusPostId, 0);
        if (row >= 0) {
            list->scrollToItem(list->item(row), QAbstractItemView::PositionAtCenter);
            focused = true;
        }
    }
    if (!focused) {
        list->scrollToBottom();
    }

    scheduleMeasurementPass();
    schedulePaintResume(renderId);
    QTimer::singleShot(0, this, &ChannelTimelineController::scheduleViewportCheck);
}

void ChannelTimelineController::schedulePaintResume(quint64 renderId)
{
    QPointer<PostsListWidget> list(area.ui ? area.ui->listWidget : nullptr);
    QTimer::singleShot(0, this, [this, list, renderId] {
        QTimer::singleShot(0, this, [this, list, renderId] {
            QTimer::singleShot(0, this, [this, list, renderId] {
                if (!list || renderGeneration != renderId) {
                    return;
                }
                list->setUpdatesEnabled(true);
                list->viewport()->update();
            });
        });
    });
}

void ChannelTimelineController::updateGapHeights()
{
    if (!active || !area.ui || !area.ui->listWidget) {
        return;
    }

    PostsListWidget* list = area.ui->listWidget;
    const int average = timeline.estimatedRowHeight();
    bool hasGap = false;
    for (int row = 0; row < list->count(); ++row) {
        if (PostsListWidget::isGapItem(list->item(row))) {
            hasGap = true;
            break;
        }
    }
    if (!hasGap) {
        lastAppliedGapRowHeight = average;
        return;
    }

    list->commitCurrentViewportAsAnchor();
    const quint64 renderId = ++renderGeneration;
    list->setUpdatesEnabled(false);
    for (int row = 0; row < list->count(); ++row) {
        QListWidgetItem* item = list->item(row);
        if (!PostsListWidget::isGapItem(item)) {
            continue;
        }

        const int count = item->data(ItemRole::gapCount).toInt();
        const qint64 height = static_cast<qint64>(std::max(0, count)) * average;
        item->setSizeHint(QSize(0, static_cast<int>(std::min<qint64>(height, INT_MAX))));
    }
    list->resizeToBottom();
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
    if (difference * 20 >= baseline) {
        updateGapHeights();
    }
}

} // namespace Mattermost
