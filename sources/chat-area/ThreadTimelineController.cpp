#include "ThreadTimelineController.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>

#include <QAbstractItemView>
#include <QListWidgetItem>
#include <QPointer>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTimer>

#include "ChatArea.h"
#include "PostsListWidget.h"
#include "ThreadTimelinePolicy.h"
#include "backend/PostTimelineService.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "post/PostWidget.h"
#include "ui_ChatArea.h"

namespace Mattermost {
namespace {

constexpr int ThreadPageSize = 30;
constexpr int SeekSeedSize = 10;
constexpr int SeekDebounceMs = 100;
constexpr int MeasurementDebounceMs = 180;
constexpr int GapPrefetchScreens = 1;

int countCachedThreadPosts(const BackendChannel& channel, const QString& rootId)
{
    int count = 0;
    for (const BackendPost& post : channel.posts) {
        if (post.id == rootId || post.root_id == rootId) {
            ++count;
        }
    }
    return count;
}

int expectedThreadPostCount(const BackendPost* root,
                            const BackendChannel& channel,
                            const QString& rootId)
{
    if (!root) {
        return std::max(1, countCachedThreadPosts(channel, rootId));
    }

    const int64_t boundedReplies = std::min<int64_t>(root->reply_count, INT_MAX - 1);
    return std::max(1, static_cast<int>(boundedReplies) + 1);
}

} // namespace

ThreadTimelineController* createThreadTimelineController(ChatArea& area)
{
    if (!area.isThread) {
        return nullptr;
    }
    return new ThreadTimelineController(area);
}

ThreadTimelineController::ThreadTimelineController(ChatArea& sourceArea)
    : QObject(&sourceArea)
    , area(sourceArea)
    , rootId(sourceArea.root_id)
{
    seekTimer.setSingleShot(true);
    seekTimer.setInterval(SeekDebounceMs);
    connect(&seekTimer, &QTimer::timeout, this, [this] {
        seekState.markReady();
        resumeSeekIfReady();
    });

    measurementTimer.setSingleShot(true);
    measurementTimer.setInterval(MeasurementDebounceMs);
    connect(&measurementTimer, &QTimer::timeout,
            this, &ThreadTimelineController::measureRenderedPosts);

    QTimer::singleShot(0, this, &ThreadTimelineController::start);
}

void ThreadTimelineController::start()
{
    if (rootId.isEmpty() || !area.ui || !area.ui->listWidget) {
        return;
    }

    seekState.reset();

    // The sparse controller owns all thread row materialization. Keep the
    // surrounding ChatArea wiring for edit/reaction/input/follow state only.
    QObject::disconnect(&area.channel, &BackendChannel::onNewPosts,
                        &area, &ChatArea::fillChannelPosts);
    QObject::disconnect(&area.channel, &BackendChannel::onNewPost,
                        &area, &ChatArea::appendChannelPost);

    BackendPost* root = area.channel.postIdToPost.value(rootId, nullptr);
    const int currentExpectedCount = expectedThreadPostCount(root, area.channel, rootId);

    ViewportAnchor restoredAnchor;
    const bool restoredState = restoreSavedState(restoredAnchor);
    bool restoredBottomNeedsCatchup = false;
    if (restoredState) {
        const int restoredTimelineCount = timeline.totalCount();
        restoredBottomNeedsCatchup = restoredAnchor.kind == ViewportAnchor::Bottom
            && currentExpectedCount > restoredTimelineCount;

        expectedPostCount = std::max(expectedPostCount, currentExpectedCount);
        // When a thread was closed while stuck to bottom, do not immediately
        // enlarge its saved geometry with an empty trailing gap. Reopen at the
        // last real row and let cursor paging append missed replies atomically.
        if (!restoredBottomNeedsCatchup
            && timeline.totalCount() != expectedPostCount) {
            timeline.setTotalCount(expectedPostCount);
        }
        if (root && !timeline.contains(rootId)) {
            timeline.placeWindow(0, QStringList {rootId});
        }
        if (nextLogicalIndex < expectedPostCount) {
            hasNext = true;
        }
        initialPagesRemaining = 0;
        initialPrefetchDone = true;
    } else {
        expectedPostCount = currentExpectedCount;
        timeline.reset(expectedPostCount);
        if (root) {
            timeline.placeWindow(0, QStringList {rootId});
            nextLogicalIndex = 1;
        }
        initialPagesRemaining = 1;
    }
    lastAppliedGapRowHeight = timeline.estimatedRowHeight();

    QScrollBar* scrollBar = area.ui->listWidget->verticalScrollBar();
    connect(scrollBar, &QScrollBar::valueChanged, this,
            [this, scrollBar](int) {
        // Only a physical thumb drag defines a random-seek target here.
        // Programmatic value changes caused by insertion, gap resizing or
        // anchor restoration must never feed back into another network seek.
        if (scrollBar->isSliderDown()) {
            updateSeekTargetFromScrollbar(false);
        }
    });
    connect(scrollBar, &QScrollBar::sliderReleased, this,
            [this] {
        updateSeekTargetFromScrollbar(true);
        persistState();
        schedulePrune();
    });
    connect(area.ui->listWidget, &PostsListWidget::userViewportChanged, this,
            [this](bool) {
        persistState();
        schedulePrune();
        scheduleViewportCheck();
    });

    connect(&area.channel, &BackendChannel::onNewPost, this,
            [this](BackendPost& post) {
        if (post.root_id != rootId) {
            return;
        }

        const ViewportAnchor anchor = captureViewportAnchor();
        const bool alreadyMaterialized = timeline.contains(post.id);
        BackendPost* rootPost = area.channel.postIdToPost.value(rootId, nullptr);
        const int reportedExpected = expectedThreadPostCount(
            rootPost, area.channel, rootId);
        expectedPostCount = threadExpectedCountAfterLiveReply(
            expectedPostCount, reportedExpected, alreadyMaterialized);
        timeline.setTotalCount(expectedPostCount);
        if (!alreadyMaterialized) {
            timeline.placeWindow(expectedPostCount - 1, QStringList {post.id});
        }
        if (initialPrefetchDone) {
            renderTimeline(QString(), false, anchor);
        }
        schedulePrune();
    });

    connect(&area.channel, &BackendChannel::onPostEdited, this,
            [this](BackendPost& post) {
        if (post.id != rootId) {
            return;
        }

        const int newExpected = std::max(
            expectedPostCount,
            expectedThreadPostCount(&post, area.channel, rootId));
        if (newExpected != expectedPostCount) {
            expectedPostCount = newExpected;
            timeline.setTotalCount(expectedPostCount);
        }
        if (initialPrefetchDone) {
            scheduleMeasurementPass();
        }
    });

    if (restoredState) {
        renderTimeline(QString(), false, restoredAnchor);
        if (restoredBottomNeedsCatchup && hasNext) {
            QTimer::singleShot(0, this, &ThreadTimelineController::requestNextPage);
        }
        return;
    }

    // Show a known root immediately while the first compact page is in flight.
    renderTimeline(root ? rootId : QString(), true);
    requestNextPage();
}

void ThreadTimelineController::requestNextPage()
{
    if (requestInFlight || !hasNext || rootId.isEmpty()) {
        return;
    }

    requestInFlight = true;
    const QString requestedCursor = cursorPostId;
    const uint64_t requestedCreateAt = cursorCreateAt;
    QPointer<ThreadTimelineController> guard(this);

    PostTimelineService::instance(area.backend).loadThreadPage(
        area.channel, rootId, ThreadPageSize, requestedCursor, requestedCreateAt,
        [guard, requestedCursor](const PostTimelineService::Page& page) {
            if (!guard) {
                return;
            }

            guard->requestInFlight = false;
            if (!page.success) {
                guard->resumeSeekIfReady();
                return;
            }

            const ViewportAnchor anchor = guard->captureViewportAnchor();
            QStringList pageIds = page.postIds;
            if (guard->nextLogicalIndex > 0) {
                pageIds.removeAll(guard->rootId);
            }

            if (pageIds.isEmpty()) {
                guard->hasNext = false;
                guard->expectedPostCount = std::max(1, guard->nextLogicalIndex);
                guard->timeline.setTotalCount(guard->expectedPostCount);
                guard->initialPrefetchDone = true;
                if (!guard->area.ui->listWidget->verticalScrollBar()->isSliderDown()) {
                    guard->renderTimeline(QString(), false, anchor);
                }
                guard->resumeSeekIfReady();
                return;
            }

            const QString newCursor = pageIds.back();
            if (!requestedCursor.isEmpty() && newCursor == requestedCursor) {
                guard->hasNext = false;
                guard->expectedPostCount = std::max(1, guard->nextLogicalIndex);
                guard->timeline.setTotalCount(guard->expectedPostCount);
                guard->initialPrefetchDone = true;
                if (!guard->area.ui->listWidget->verticalScrollBar()->isSliderDown()) {
                    guard->renderTimeline(QString(), false, anchor);
                }
                guard->resumeSeekIfReady();
                return;
            }

            const int pageSize = static_cast<int>(pageIds.size());
            const int neededCount = guard->nextLogicalIndex > INT_MAX - pageSize
                ? INT_MAX : guard->nextLogicalIndex + pageSize;
            if (neededCount > guard->timeline.totalCount()) {
                guard->expectedPostCount = std::max(guard->expectedPostCount, neededCount);
                guard->timeline.setTotalCount(guard->expectedPostCount);
            }

            guard->timeline.placeWindow(guard->nextLogicalIndex, pageIds);
            guard->nextLogicalIndex = neededCount;
            guard->cursorPostId = newCursor;
            if (BackendPost* cursorPost = guard->area.channel.postIdToPost.value(newCursor, nullptr)) {
                guard->cursorCreateAt = cursorPost->create_at;
            }

            const int responseSize = static_cast<int>(page.postIds.size());
            if (threadPageConfirmsNewestBoundary(page.hasNext, page.nextPostId,
                                                 responseSize, ThreadPageSize)) {
                // Server pagination is authoritative here. reply_count can race
                // a websocket update by one event, so a confirmed newest edge
                // must collapse any speculative trailing logical slots.
                guard->expectedPostCount = std::max(1, guard->nextLogicalIndex);
                guard->timeline.setTotalCount(guard->expectedPostCount);
                guard->hasNext = false;
            } else {
                guard->hasNext = guard->nextLogicalIndex < guard->expectedPostCount
                    && (page.hasNext || responseSize >= ThreadPageSize);
            }

            if (guard->initialPagesRemaining > 0) {
                --guard->initialPagesRemaining;
            }
            if (guard->initialPagesRemaining > 0 && guard->hasNext) {
                QTimer::singleShot(0, guard, &ThreadTimelineController::requestNextPage);
                return;
            }

            guard->initialPrefetchDone = true;
            if (!guard->area.ui->listWidget->verticalScrollBar()->isSliderDown()) {
                guard->renderTimeline(QString(), false, anchor);
                guard->scheduleViewportCheck();
            }
            guard->schedulePrune();
            guard->resumeSeekIfReady();
        });
}

void ThreadTimelineController::updateSeekTargetFromScrollbar(bool readyImmediately)
{
    if (!area.ui || !area.ui->listWidget || timeline.totalCount() <= 1) {
        return;
    }

    QScrollBar* bar = area.ui->listWidget->verticalScrollBar();
    int target = timeline.logicalIndexForScrollPosition(
        bar->value(), bar->minimum(), bar->maximum());
    target = std::max(1, std::min(timeline.totalCount() - 1, target));

    if (seekState.setTarget(target)) {
        pendingSeekIndex = target;
    }

    if (readyImmediately) {
        seekTimer.stop();
        seekState.markReady();
        resumeSeekIfReady();
    } else {
        seekTimer.start();
    }
}

void ThreadTimelineController::resumeSeekIfReady()
{
    if (!seekState.isReady() || requestInFlight) {
        return;
    }
    requestSeek(seekState.currentTicket());
}

void ThreadTimelineController::requestSeek(const TimelineSeekState::Ticket& ticket)
{
    if (!seekState.isCurrent(ticket) || requestInFlight
        || ticket.targetIndex <= 0 || ticket.targetIndex >= timeline.totalCount()) {
        return;
    }

    pendingSeekIndex = ticket.targetIndex;
    if (!timeline.postIdAt(ticket.targetIndex).isEmpty()) {
        const QString targetId = timeline.postIdAt(ticket.targetIndex);
        seekState.complete(ticket);
        pendingSeekIndex = -1;
        renderTimeline(targetId, false, ViewportAnchor());
        return;
    }

    BackendPost* root = area.channel.postIdToPost.value(rootId, nullptr);
    if (!root || root->last_reply_at <= root->create_at || expectedPostCount <= 1) {
        seekState.complete(ticket);
        pendingSeekIndex = -1;
        requestNextPage();
        return;
    }

    const long double fraction = static_cast<long double>(ticket.targetIndex)
        / static_cast<long double>(std::max(1, expectedPostCount - 1));
    const long double span = static_cast<long double>(root->last_reply_at - root->create_at);
    const uint64_t estimatedCreateAt = root->create_at
        + static_cast<uint64_t>(span * fraction);

    requestInFlight = true;
    QPointer<ThreadTimelineController> guard(this);
    PostTimelineService::instance(area.backend).loadThreadFromTime(
        area.channel, rootId, SeekSeedSize, estimatedCreateAt,
        [guard, ticket](const PostTimelineService::Page& page) {
            if (!guard) {
                return;
            }
            guard->requestInFlight = false;

            // PostTimelineService has already ingested the response into the
            // BackendChannel memory cache. If the user moved the thumb meanwhile,
            // keep that data cached but never mutate the visible sparse topology.
            if (!guard->seekState.isCurrent(ticket)) {
                guard->resumeSeekIfReady();
                return;
            }
            if (!page.success || page.postIds.isEmpty()) {
                guard->seekState.complete(ticket);
                guard->pendingSeekIndex = -1;
                return;
            }

            QStringList ids = page.postIds;
            ids.removeAll(guard->rootId);
            if (ids.isEmpty()) {
                guard->seekState.complete(ticket);
                guard->pendingSeekIndex = -1;
                return;
            }

            // Timestamp seek is approximate. Never overwrite an existing loaded
            // span just because the time-to-index estimate was imperfect: fit the
            // seed into the nearest sparse gap and merge later cursor blocks into
            // that local window.
            PostTimeline::LogicalWindow window = guard->timeline.gapWindowNear(
                ticket.targetIndex, static_cast<int>(ids.size()), 1);
            if (!window.isValid()) {
                guard->seekState.complete(ticket);
                guard->pendingSeekIndex = -1;
                return;
            }
            if (static_cast<int>(ids.size()) > window.count) {
                ids = ids.mid(0, window.count);
            }
            guard->timeline.placeWindow(window.firstIndex, ids);

            int focusIndex = std::max(window.firstIndex,
                std::min(window.lastIndex(), ticket.targetIndex));
            QString focusId = guard->timeline.postIdAt(focusIndex);
            if (focusId.isEmpty() && !ids.isEmpty()) {
                focusId = ids.at(ids.size() / 2);
            }

            guard->seekState.complete(ticket);
            guard->pendingSeekIndex = -1;
            guard->renderTimeline(focusId, false, ViewportAnchor());
            guard->schedulePrune();
            guard->persistState();
        });
}

ThreadTimelineController::ViewportAnchor ThreadTimelineController::captureViewportAnchor() const
{
    ViewportAnchor anchor;
    if (!area.ui || !area.ui->listWidget) {
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
    anchor.logicalIndex = location.logicalIndex;
    anchor.offsetWithinEstimatedRow = location.offsetWithinRow;
    return anchor;
}

void ThreadTimelineController::restoreViewportAnchor(const ViewportAnchor& anchor,
                                                     const QString& focusPostId,
                                                     bool focusAtTop)
{
    if (!area.ui || !area.ui->listWidget) {
        return;
    }

    PostsListWidget* list = area.ui->listWidget;
    if (!focusPostId.isEmpty()) {
        const int row = list->findPostByIndex(focusPostId, 0);
        if (row >= 0) {
            const int topOffset = focusAtTop ? 0 : std::max(0, list->viewport()->height() / 2);
            list->finishTimelineRebuildAtPost(focusPostId, topOffset);
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
        const int index = std::max(0, std::min(timeline.totalCount() - 1, anchor.logicalIndex));
        const qint64 centerPixel = timeline.estimatedPixelForIndex(index)
            + anchor.offsetWithinEstimatedRow;
        list->finishTimelineRebuildAtPixel(
            centerPixel - list->viewport()->height() / 2);
        return;
    }

    list->finishTimelineRebuildAtBottom();
}

void ThreadTimelineController::scheduleViewportCheck()
{
    if (viewportCheckScheduled || rebuilding) {
        return;
    }
    viewportCheckScheduled = true;
    QTimer::singleShot(0, this, [this] {
        viewportCheckScheduled = false;
        checkViewport();
    });
}

int ThreadTimelineController::logicalIndexNearViewport(int extraScreens,
                                                       bool* viewportCenterInsideGap) const
{
    if (viewportCenterInsideGap) {
        *viewportCenterInsideGap = false;
    }
    if (!area.ui || !area.ui->listWidget || timeline.totalCount() <= 0) {
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
    if (viewportCenterInsideGap) {
        *viewportCenterInsideGap = center.isValid() && !center.loaded;
    }
    if (center.isValid() && !center.loaded) {
        return center.logicalIndex;
    }

    const qint64 margin = static_cast<qint64>(viewportHeight)
        * std::max(1, extraScreens);
    const qint64 probes[] = {
        std::max<qint64>(0, static_cast<qint64>(bar->value()) - margin),
        std::max<qint64>(0, static_cast<qint64>(bar->value()) - viewportHeight),
        static_cast<qint64>(bar->value()),
        static_cast<qint64>(bar->value()) + viewportHeight,
        static_cast<qint64>(bar->value()) + viewportHeight + margin,
    };

    int bestIndex = -1;
    qint64 bestDistance = LLONG_MAX;
    for (qint64 probe : probes) {
        const PostTimeline::PixelLocation location = timeline.locatePixel(probe);
        if (!location.isValid() || location.loaded) {
            continue;
        }
        const qint64 distance = std::llabs(probe - centerPixel);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestIndex = location.logicalIndex;
        }
    }
    return bestIndex;
}

void ThreadTimelineController::checkViewport()
{
    if (!area.ui || !area.ui->listWidget) {
        return;
    }

    PostsListWidget* list = area.ui->listWidget;
    QScrollBar* scrollBar = list->verticalScrollBar();
    const int viewportHeight = list->viewport()->height();

    if (scrollBar->isSliderDown()) {
        updateSeekTargetFromScrollbar(false);
        return;
    }

    if (requestInFlight) {
        return;
    }

    if (hasNext && nextLogicalIndex < timeline.totalCount()) {
        const qint64 prefixBoundary = timeline.estimatedPixelForIndex(nextLogicalIndex);
        const qint64 viewportBottom = static_cast<qint64>(scrollBar->value()) + viewportHeight;
        if (viewportBottom + static_cast<qint64>(viewportHeight) * GapPrefetchScreens
            >= prefixBoundary) {
            requestNextPage();
            return;
        }
    }

    bool centerInsideGap = false;
    const int targetIndex = logicalIndexNearViewport(GapPrefetchScreens, &centerInsideGap);
    if (targetIndex < 0) {
        return;
    }

    // Only an unloaded row *after* the sequential cursor belongs to the next
    // cursor page. Gaps before nextLogicalIndex can be created by the 200-row
    // materialization budget and must be reloaded by random seek instead.
    if (targetIndex >= nextLogicalIndex
        && targetIndex - nextLogicalIndex < ThreadPageSize) {
        if (hasNext) {
            requestNextPage();
        }
        return;
    }

    seekState.setTarget(targetIndex);
    pendingSeekIndex = targetIndex;
    seekState.markReady();
    resumeSeekIfReady();
}

void ThreadTimelineController::renderTimeline(const QString& focusPostId,
                                              bool focusAtTop,
                                              const ViewportAnchor& requestedAnchor)
{
    if (!area.ui || !area.ui->listWidget || rebuilding) {
        return;
    }

    ViewportAnchor anchor = requestedAnchor;
    if (!anchor.isValid() && initialPrefetchDone) {
        anchor = captureViewportAnchor();
    }

    rebuilding = true;
    const quint64 renderId = ++renderGeneration;
    PostsListWidget* list = area.ui->listWidget;
    list->setUpdatesEnabled(false);
    const QSignalBlocker scrollSignals(list->verticalScrollBar());
    list->beginTimelineRebuild();

    BackendPost* lastRootPost = nullptr;
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

        for (const QString& postId : span.postIds) {
            BackendPost* post = area.channel.postIdToPost.value(postId, nullptr);
            if (!post || (post->id != rootId && post->root_id != rootId)) {
                continue;
            }

            auto* postWidget = new PostWidget(area.backend, *post, list, &area, lastRootPost);
            list->insertPost(postWidget);
            lastRootPost = post->rootPost;
            connect(postWidget, &PostWidget::dimensionsChanged,
                    this, &ThreadTimelineController::scheduleMeasurementPass);
        }
    }

    rebuilding = false;
    restoreViewportAnchor(anchor, focusPostId, focusAtTop);
    scheduleMeasurementPass();
    schedulePaintResume(renderId);
    schedulePrune();
    QTimer::singleShot(0, this, [this] {
        persistState();
        // Network prefetch is intentionally driven only by user viewport input
        // or an explicit page callback. Reconciliation/layout must not start a
        // new seek by itself; that feedback loop caused unbounded fetch/render.
    });
}

void ThreadTimelineController::schedulePaintResume(quint64 renderId)
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

void ThreadTimelineController::updateGapHeights()
{
    if (!area.ui || !area.ui->listWidget) {
        return;
    }

    PostsListWidget* list = area.ui->listWidget;
    const ViewportAnchor anchor = captureViewportAnchor();
    if (anchor.kind == ViewportAnchor::Gap) {
        return;
    }

    const int average = timeline.estimatedRowHeight();
    bool changed = false;
    const quint64 renderId = ++renderGeneration;
    list->setUpdatesEnabled(false);
    const QSignalBlocker scrollSignals(list->verticalScrollBar());
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

void ThreadTimelineController::scheduleMeasurementPass()
{
    if (rebuilding) {
        return;
    }
    measurementTimer.start();
}

void ThreadTimelineController::measureRenderedPosts()
{
    if (!area.ui || !area.ui->listWidget || rebuilding) {
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
