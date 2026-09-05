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
    pendingSeekIndex = -1;
    seekPreserveViewport = false;
    seekViewportAnchor = ViewportAnchor();

    QObject::disconnect(&area.channel, &BackendChannel::onNewPosts,
                        &area, &ChatArea::fillChannelPosts);
    QObject::disconnect(&area.channel, &BackendChannel::onNewPost,
                        &area, &ChatArea::appendChannelPost);

    BackendPost* root = area.channel.postIdToPost.value(rootId, nullptr);
    const int currentExpectedCount = expectedThreadPostCount(root, area.channel, rootId);

    // Reuse saved sparse topology as a memory optimization, but never restore an
    // old viewport merely because the thread window was opened before. Ordinary
    // Mattermost thread opening means newest; explicit Attention/Recent/permalink
    // navigation installs its own semantic target before the deferred tail open.
    ViewportAnchor ignoredSavedAnchor;
    const bool restoredState = restoreSavedState(ignoredSavedAnchor);
    if (restoredState) {
        expectedPostCount = std::max(expectedPostCount, currentExpectedCount);
        if (timeline.totalCount() != expectedPostCount) {
            timeline.setTotalCount(expectedPostCount);
        }
        if (root && !timeline.contains(rootId)) {
            timeline.placeWindow(0, QStringList {rootId});
        }
    } else {
        expectedPostCount = currentExpectedCount;
        timeline.reset(expectedPostCount);
        if (root) {
            timeline.placeWindow(0, QStringList {rootId});
        }
    }

    // Sequential root->newest paging is retained only as a fallback for servers
    // without useful thread timestamps. Normal opening and ordinary gap loading
    // use newest-tail/random-seek windows instead, so an old prefix can never win
    // a race and move a freshly opened thread back to its first replies.
    nextLogicalIndex = std::max(1, nextLogicalIndex);
    initialPagesRemaining = 0;
    initialPrefetchDone = true;
    hasNext = false;
    lastAppliedGapRowHeight = timeline.estimatedRowHeight();

    QScrollBar* scrollBar = area.ui->listWidget->verticalScrollBar();
    connect(scrollBar, &QScrollBar::valueChanged, this,
            [this, scrollBar](int) {
        if (scrollBar->isSliderDown()) {
            updateSeekTargetFromScrollbar(false);
        }
    });
    connect(scrollBar, &QScrollBar::sliderReleased, this,
            [this] {
        updateSeekTargetFromScrollbar(true);
        persistState();
    });
    connect(area.ui->listWidget, &PostsListWidget::userViewportChanged, this,
            [this](bool) {
        persistState();
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

    // Queue the ordinary newest-edge policy behind this start. A semantic target
    // queued by MainWindow for Attention/Recent/permalink navigation runs first
    // and its navigation lock makes openNewestOnInitialOpen() a no-op.
    QTimer::singleShot(0, this, &ThreadTimelineController::openNewestOnInitialOpen);
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
                guard->resumeSeekIfReady();
                return;
            }

            const QString newCursor = pageIds.back();
            if (!requestedCursor.isEmpty() && newCursor == requestedCursor) {
                guard->hasNext = false;
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
            guard->hasNext = !threadPageConfirmsNewestBoundary(
                page.hasNext, page.nextPostId, responseSize, ThreadPageSize)
                && guard->nextLogicalIndex < guard->expectedPostCount;

            if (!guard->area.ui->listWidget->verticalScrollBar()->isSliderDown()) {
                guard->renderTimeline(QString(), false, anchor);
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

    seekPreserveViewport = false;
    seekViewportAnchor = ViewportAnchor();
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
    requestSeekSeed(ticket);
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

    if (scrollBar->isSliderDown()) {
        updateSeekTargetFromScrollbar(false);
        return;
    }

    bool centerInsideGap = false;
    const int targetIndex = logicalIndexNearViewport(GapPrefetchScreens, &centerInsideGap);
    if (targetIndex < 0) {
        return;
    }

    // Wheel/local scrolling preserves the concrete viewport. This is distinct
    // from a thumb random seek, which deliberately centres its logical target.
    // Updating the generation even while an older request is in flight makes
    // that response cache-only and lets the newer user intent win afterwards.
    seekPreserveViewport = true;
    seekViewportAnchor = captureViewportAnchor();
    if (seekState.setTarget(targetIndex)) {
        pendingSeekIndex = targetIndex;
    }
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
    const ViewportAnchor anchor = seekState.hasActiveTransaction() && seekPreserveViewport
        ? seekViewportAnchor : captureViewportAnchor();
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
