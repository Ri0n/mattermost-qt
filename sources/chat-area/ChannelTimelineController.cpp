#include "ChannelTimelineController.h"

#include <algorithm>
#include <climits>
#include <cmath>

#include <QAbstractItemView>
#include <QDateTime>
#include <QEvent>
#include <QListWidgetItem>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>

#include "ChatArea.h"
#include "PostsListWidget.h"
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

    area.installEventFilter(this);
    QTimer::singleShot(0, this, &ChannelTimelineController::tryStart);
}

bool ChannelTimelineController::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == &area) {
        if (event->type() == QEvent::Show) {
            QTimer::singleShot(0, this, &ChannelTimelineController::tryStart);
        } else if (event->type() == QEvent::Hide) {
            deactivate();
        }
    }
    return QObject::eventFilter(watched, event);
}

void ChannelTimelineController::tryStart()
{
    if (active || area.isThread || !area.initialized || !area.ui || !area.ui->listWidget) {
        return;
    }
    start();
}

int ChannelTimelineController::channelRootPostCount() const
{
    const int reported = area.channel.has_total_msg_count_root
        ? area.channel.total_msg_count_root
        : area.channel.total_msg_count;
    return std::max(0, reported);
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

    expectedPostCount = channelRootPostCount();
    timeline.reset(expectedPostCount);

    // The sparse controller owns bulk history materialization. Keep the legacy
    // onNewPost path: it contains established unread/new-message semantics and
    // appends a websocket root post correctly at the newest edge. Bulk REST
    // responses, however, must have exactly one row owner.
    QObject::disconnect(&area.channel, &BackendChannel::onNewPosts,
                        &area, &ChatArea::fillChannelPosts);

    // Disable the two legacy paging lambdas without disconnecting unrelated
    // PostsListWidget -> ChatArea signals such as read tracking. Both lambdas
    // already guard on this flag; the controller keeps it true while active.
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

        expectedPostCount = std::max(expectedPostCount + 1,
                                     timeline.totalCount() + 1);
        timeline.setTotalCount(expectedPostCount);
        timeline.placeWindow(expectedPostCount - 1, QStringList {post.id});
        scheduleMeasuredHeightUpdate(post.id);
        // Appending one loaded row while also increasing totalCount by one does
        // not change the number of virtual rows, so existing gap geometry stays
        // valid. The legacy appendChannelPost() slot owns the concrete row.
    });

    renderTimeline();

    const int pages = expectedPostCount > 0
        ? std::max(1, (expectedPostCount + ChannelPageSize - 1) / ChannelPageSize)
        : LargeChannelPrefetchPages;
    initialPageTarget = std::min(
        pages,
        expectedPostCount > ChannelPageSize * SmallChannelPrefetchPages
            ? LargeChannelPrefetchPages
            : SmallChannelPrefetchPages);
    initialPageTarget = std::max(1, initialPageTarget);

    continueInitialPrefetch();
}

void ChannelTimelineController::deactivate()
{
    if (!active) {
        return;
    }

    active = false;
    ++generation;
    seekTimer.stop();
    pendingSeekIndex = -1;
    requestInFlight = false;
    area.gettingOlderPosts = false;

    QObject::disconnect(&area.channel, nullptr, this, nullptr);
    if (area.ui) {
        if (area.ui->loadOldPosts) {
            QObject::disconnect(area.ui->loadOldPosts, nullptr, this, nullptr);
        }
        if (area.ui->listWidget) {
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

    if (expectedPostCount > 0
        && static_cast<qint64>(page) * ChannelPageSize >= expectedPostCount) {
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
            if (ids.isEmpty()) {
                guard->loadedPages.insert(page);
                guard->initialPageTarget = std::min(guard->initialPageTarget, page + 1);
                while (guard->loadedPages.contains(guard->nextOlderPage)) {
                    ++guard->nextOlderPage;
                }
                guard->flushDeferredExternalPosts();
                guard->continueInitialPrefetch();
                return;
            }

            const int responseSize = static_cast<int>(ids.size());
            const int minimumKnownCount = page * ChannelPageSize + responseSize;
            int reportedCount = guard->channelRootPostCount();
            if (reportedCount <= 0) {
                // A full page without channel count means there may be another
                // page. Keep one provisional page of virtual geometry until a
                // short/empty response gives us a real oldest edge.
                reportedCount = minimumKnownCount
                    + (responseSize >= ChannelPageSize ? ChannelPageSize : 0);
            }
            guard->expectedPostCount = std::max(
                guard->expectedPostCount,
                std::max(minimumKnownCount, reportedCount));
            guard->timeline.setTotalCount(guard->expectedPostCount);

            const int firstIndex = guard->authoritativeFirstIndex(page, responseSize);
            guard->timeline.placeWindow(firstIndex, ids);
            guard->loadedPages.insert(page);
            while (guard->loadedPages.contains(guard->nextOlderPage)) {
                ++guard->nextOlderPage;
            }

            QString focusId;
            if (focusIndex >= 0) {
                const int clamped = std::max(firstIndex,
                    std::min(focusIndex, firstIndex + responseSize - 1));
                focusId = guard->timeline.postIdAt(clamped);
            }

            guard->renderTimeline(focusId);
            guard->flushDeferredExternalPosts();
            guard->continueInitialPrefetch();
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

    scheduleViewportCheck();
}

void ChannelTimelineController::requestOlderPage()
{
    if (!active || requestInFlight) {
        return;
    }
    if (expectedPostCount > 0
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

    if (requestInFlight) {
        deferredExternalPostIds.append(ids);
        if (!deferredExternalFlushScheduled) {
            deferredExternalFlushScheduled = true;
            QTimer::singleShot(0, this, [this] {
                deferredExternalFlushScheduled = false;
                if (!requestInFlight) {
                    flushDeferredExternalPosts();
                }
            });
        }
        return;
    }

    placeApproximateWindow(ids);
}

void ChannelTimelineController::flushDeferredExternalPosts()
{
    if (!active || requestInFlight || deferredExternalPostIds.isEmpty()) {
        return;
    }

    const QStringList ids = deferredExternalPostIds;
    deferredExternalPostIds.clear();
    placeApproximateWindow(ids);
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

        const int minCenter = span.firstIndex + count / 2;
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

        Q_UNUSED(minCenter);
    }
    return bestFirst;
}

void ChannelTimelineController::placeApproximateWindow(const QStringList& postIds,
                                                        const QString& focusPostId)
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
        if (!focusPostId.isEmpty()) {
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
    renderTimeline(focusPostId);
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

QListWidgetItem* ChannelTimelineController::gapAtViewportCenter() const
{
    if (!active || !area.ui || !area.ui->listWidget) {
        return nullptr;
    }
    PostsListWidget* list = area.ui->listWidget;
    QListWidgetItem* item = list->itemAt(list->viewport()->rect().center());
    return PostsListWidget::isGapItem(item) ? item : nullptr;
}

int ChannelTimelineController::logicalIndexInsideGap(const QListWidgetItem* gapItem) const
{
    if (!gapItem || !area.ui || !area.ui->listWidget) {
        return -1;
    }

    PostsListWidget* list = area.ui->listWidget;
    const QRect rect = list->visualItemRect(const_cast<QListWidgetItem*>(gapItem));
    if (!rect.isValid()) {
        return -1;
    }

    const int first = gapItem->data(ItemRole::gapFirstIndex).toInt();
    const int count = gapItem->data(ItemRole::gapCount).toInt();
    if (count <= 0) {
        return -1;
    }

    const int average = std::max(1, timeline.estimatedRowHeight());
    const int yInside = std::max(0, list->viewport()->rect().center().y() - rect.top());
    const int offset = std::min(count - 1, yInside / average);
    return first + offset;
}

int ChannelTimelineController::nearbyGapLogicalIndex() const
{
    if (!active || !area.ui || !area.ui->listWidget) {
        return -1;
    }

    PostsListWidget* list = area.ui->listWidget;
    const int viewportHeight = list->viewport()->height();
    const int margin = viewportHeight * 2;
    int bestIndex = -1;
    int bestDistance = INT_MAX;

    for (int row = 0; row < list->count(); ++row) {
        QListWidgetItem* item = list->item(row);
        if (!PostsListWidget::isGapItem(item)) {
            continue;
        }

        const QRect rect = list->visualItemRect(item);
        if (!rect.isValid()) {
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

        int candidate = first;
        int distance = std::abs(rect.top() - viewportHeight);
        if (rect.top() < 0) {
            candidate = first + count - 1;
            distance = std::abs(rect.bottom());
        }

        if (distance < bestDistance) {
            bestDistance = distance;
            bestIndex = candidate;
        }
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

    if (QListWidgetItem* gap = gapAtViewportCenter()) {
        const int targetIndex = logicalIndexInsideGap(gap);
        if (targetIndex >= 0) {
            if (scrollBar->isSliderDown()) {
                pendingSeekIndex = targetIndex;
                seekTimer.start();
            } else {
                requestSeek(targetIndex);
            }
        }
        return;
    }

    pendingSeekIndex = -1;
    seekTimer.stop();

    const int nearGap = nearbyGapLogicalIndex();
    if (nearGap >= 0) {
        requestPageForIndex(nearGap, false);
    }
}

void ChannelTimelineController::renderTimeline(const QString& focusPostId)
{
    if (!active || !area.ui || !area.ui->listWidget || rebuilding) {
        return;
    }

    rebuilding = true;
    PostsListWidget* list = area.ui->listWidget;
    list->commitCurrentViewportAsAnchor();
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
            // Geometry only: no text, icon, brush or child widget. The normal
            // channel viewport background remains visible through large gaps.
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
            scheduleMeasuredHeightUpdate(postId);
            connect(postWidget, &PostWidget::dimensionsChanged, this,
                    [this, postId] { scheduleMeasuredHeightUpdate(postId); });
        }
    }

    area.areaIsFilled = timeline.loadedCount() > 20;
    area.gettingOlderPosts = true;
    rebuilding = false;

    if (!focusPostId.isEmpty()) {
        const int row = list->findPostByIndex(focusPostId, 0);
        if (row >= 0) {
            list->scrollToItem(list->item(row), QAbstractItemView::PositionAtCenter);
        }
    } else {
        // On first materialization this establishes the historical latest-post
        // viewport. On later rebuilds PostsListWidget has a concrete saved
        // anchor, so scrollToBottom() only schedules restoration of that anchor.
        list->scrollToBottom();
    }

    QTimer::singleShot(0, this, [this] {
        updateGapHeights();
        scheduleViewportCheck();
    });
}

void ChannelTimelineController::updateGapHeights()
{
    if (!active || !area.ui || !area.ui->listWidget) {
        return;
    }

    PostsListWidget* list = area.ui->listWidget;
    const int average = timeline.estimatedRowHeight();
    for (int row = 0; row < list->count(); ++row) {
        QListWidgetItem* item = list->item(row);
        if (!PostsListWidget::isGapItem(item)) {
            continue;
        }

        const int count = item->data(ItemRole::gapCount).toInt();
        const qint64 height = static_cast<qint64>(std::max(0, count)) * average;
        item->setSizeHint(QSize(0, static_cast<int>(std::min<qint64>(height, INT_MAX))));
    }
}

void ChannelTimelineController::scheduleMeasuredHeightUpdate(const QString& postId)
{
    QTimer::singleShot(0, this, [this, postId] {
        if (!active || !area.ui || !area.ui->listWidget) {
            return;
        }
        PostsListWidget* list = area.ui->listWidget;
        const int row = list->findPostByIndex(postId, 0);
        if (row < 0) {
            return;
        }
        QListWidgetItem* item = list->item(row);
        if (!item) {
            return;
        }
        const int height = item->sizeHint().height();
        if (height > 0) {
            timeline.recordMeasuredHeight(postId, height);
            updateGapHeights();
        }
    });
}

} // namespace Mattermost
