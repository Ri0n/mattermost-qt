#include "ThreadTimelineController.h"

#include <algorithm>
#include <climits>
#include <cstdlib>

#include <QAbstractItemView>
#include <QListWidgetItem>
#include <QPointer>
#include <QScrollBar>
#include <QTimer>

#include "ChatArea.h"
#include "PostsListWidget.h"
#include "backend/PostTimelineService.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "post/PostWidget.h"
#include "ui_ChatArea.h"

namespace Mattermost {
namespace {

constexpr int ThreadPageSize = 80;
constexpr int SmallThreadPrefetchPages = 2;
constexpr int LargeThreadPrefetchPages = 3;
constexpr int SeekDebounceMs = 140;
constexpr int MeasurementDebounceMs = 120;
constexpr int GapPrefetchScreens = 3;

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
        if (pendingSeekIndex >= 0) {
            requestSeek(pendingSeekIndex);
        }
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

    QObject::disconnect(&area.channel, &BackendChannel::onNewPosts,
                        &area, &ChatArea::fillChannelPosts);
    QObject::disconnect(&area.channel, &BackendChannel::onNewPost,
                        &area, &ChatArea::appendChannelPost);

    BackendPost* root = area.channel.postIdToPost.value(rootId, nullptr);
    expectedPostCount = expectedThreadPostCount(root, area.channel, rootId);

    timeline.reset(expectedPostCount);
    if (root) {
        timeline.placeWindow(0, QStringList {rootId});
        nextLogicalIndex = 1;
    }
    lastAppliedGapRowHeight = timeline.estimatedRowHeight();

    initialPagesRemaining = expectedPostCount > ThreadPageSize * SmallThreadPrefetchPages
        ? LargeThreadPrefetchPages
        : std::min(SmallThreadPrefetchPages,
                   std::max(1, (expectedPostCount + ThreadPageSize - 1) / ThreadPageSize));

    // Show the root immediately, but do not destroy/recreate the whole thread
    // after each of the 2-3 initial network pages. The prefetched pages are
    // accumulated in PostTimeline and rendered in one batch below.
    renderTimeline(root ? rootId : QString(), true);
    area.ui->listWidget->commitCurrentViewportAsAnchor();

    QScrollBar* scrollBar = area.ui->listWidget->verticalScrollBar();
    connect(scrollBar, &QScrollBar::valueChanged, this,
            [this](int) { scheduleViewportCheck(); });
    connect(scrollBar, &QScrollBar::sliderReleased, this,
            [this] { scheduleViewportCheck(); });

    connect(&area.channel, &BackendChannel::onNewPost, this,
            [this](BackendPost& post) {
        if (post.root_id != rootId) {
            return;
        }

        if (expectedPostCount < INT_MAX) {
            ++expectedPostCount;
        }
        timeline.setTotalCount(expectedPostCount);
        timeline.placeWindow(expectedPostCount - 1, QStringList {post.id});
        if (initialPrefetchDone) {
            renderTimeline();
        }
    });
    connect(&area.channel, &BackendChannel::onPostEdited, this,
            [this](BackendPost& post) {
        if (post.id != rootId) {
            return;
        }

        expectedPostCount = std::max(
            expectedPostCount,
            expectedThreadPostCount(&post, area.channel, rootId));
        timeline.setTotalCount(expectedPostCount);
        if (initialPrefetchDone) {
            scheduleMeasurementPass();
        }
    });

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
                return;
            }

            if (page.postIds.isEmpty()) {
                guard->hasNext = false;
                guard->initialPrefetchDone = true;
                guard->renderTimeline();
                guard->scheduleViewportCheck();
                return;
            }

            QStringList pageIds = page.postIds;
            if (guard->nextLogicalIndex > 0 && !pageIds.isEmpty()
                && pageIds.front() == guard->rootId) {
                pageIds.removeFirst();
            }
            if (pageIds.isEmpty()) {
                guard->hasNext = false;
                guard->initialPrefetchDone = true;
                guard->renderTimeline();
                guard->scheduleViewportCheck();
                return;
            }

            const QString newCursor = pageIds.back();
            if (!requestedCursor.isEmpty() && newCursor == requestedCursor) {
                guard->hasNext = false;
                guard->initialPrefetchDone = true;
                guard->renderTimeline();
                guard->scheduleViewportCheck();
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
            guard->hasNext = page.hasNext || responseSize >= ThreadPageSize;

            if (guard->initialPagesRemaining > 0) {
                --guard->initialPagesRemaining;
            }
            if (guard->initialPagesRemaining > 0 && guard->hasNext) {
                QTimer::singleShot(0, guard, &ThreadTimelineController::requestNextPage);
                return;
            }

            guard->initialPrefetchDone = true;
            guard->renderTimeline();
            guard->scheduleViewportCheck();
        });
}

void ThreadTimelineController::requestSeek(int logicalIndex)
{
    pendingSeekIndex = -1;
    if (requestInFlight || logicalIndex < 0 || logicalIndex >= timeline.totalCount()) {
        return;
    }
    if (!timeline.postIdAt(logicalIndex).isEmpty()) {
        return;
    }

    BackendPost* root = area.channel.postIdToPost.value(rootId, nullptr);
    if (!root || root->last_reply_at <= root->create_at || expectedPostCount <= 1) {
        requestNextPage();
        return;
    }

    const long double fraction = static_cast<long double>(logicalIndex)
        / static_cast<long double>(std::max(1, expectedPostCount - 1));
    const long double span = static_cast<long double>(root->last_reply_at - root->create_at);
    const uint64_t estimatedCreateAt = root->create_at
        + static_cast<uint64_t>(span * fraction);

    requestInFlight = true;
    QPointer<ThreadTimelineController> guard(this);
    PostTimelineService::instance(area.backend).loadThreadFromTime(
        area.channel, rootId, ThreadPageSize, estimatedCreateAt,
        [guard, logicalIndex](const PostTimelineService::Page& page) {
            if (!guard) {
                return;
            }
            guard->requestInFlight = false;
            if (!page.success || page.postIds.isEmpty()) {
                return;
            }

            const int pageSize = static_cast<int>(page.postIds.size());
            const int firstIndex = std::min(
                logicalIndex,
                std::max(0, guard->timeline.totalCount() - pageSize));
            guard->timeline.placeWindow(firstIndex, page.postIds);
            guard->renderTimeline(page.postIds.front(), false);
        });
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

int ThreadTimelineController::logicalIndexForNearbyGap(bool* viewportCenterInsideGap) const
{
    if (viewportCenterInsideGap) {
        *viewportCenterInsideGap = false;
    }
    if (!area.ui || !area.ui->listWidget) {
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
        if (!rect.isValid() || rect.height() <= 0
            || rect.bottom() < -margin || rect.top() > viewportHeight + margin) {
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

void ThreadTimelineController::checkViewport()
{
    if (requestInFlight || !area.ui || !area.ui->listWidget) {
        return;
    }

    PostsListWidget* list = area.ui->listWidget;
    QScrollBar* scrollBar = list->verticalScrollBar();
    const int viewportHeight = list->viewport()->height();

    if (!scrollBar->isSliderDown() && hasNext && nextLogicalIndex < timeline.totalCount()) {
        const qint64 prefixBoundary = timeline.estimatedPixelForIndex(nextLogicalIndex);
        const qint64 viewportBottom = static_cast<qint64>(scrollBar->value()) + viewportHeight;
        if (viewportBottom + static_cast<qint64>(viewportHeight) * GapPrefetchScreens
            >= prefixBoundary) {
            requestNextPage();
            return;
        }
    }

    bool centerInsideGap = false;
    const int targetIndex = logicalIndexForNearbyGap(&centerInsideGap);
    if (targetIndex < 0 || std::abs(targetIndex - nextLogicalIndex) < ThreadPageSize) {
        pendingSeekIndex = -1;
        seekTimer.stop();
        return;
    }

    if (scrollBar->isSliderDown() && centerInsideGap) {
        pendingSeekIndex = targetIndex;
        seekTimer.start();
        return;
    }

    if (centerInsideGap) {
        requestSeek(targetIndex);
    }
}

void ThreadTimelineController::renderTimeline(const QString& focusPostId, bool focusAtTop)
{
    if (!area.ui || !area.ui->listWidget || rebuilding) {
        return;
    }

    rebuilding = true;
    const quint64 renderId = ++renderGeneration;
    PostsListWidget* list = area.ui->listWidget;
    list->commitCurrentViewportAsAnchor();
    list->setUpdatesEnabled(false);
    list->clear();

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

    bool focused = false;
    if (!focusPostId.isEmpty()) {
        const int row = list->findPostByIndex(focusPostId, 0);
        if (row >= 0) {
            list->scrollToItem(list->item(row), focusAtTop
                ? QAbstractItemView::PositionAtTop
                : QAbstractItemView::PositionAtCenter);
            focused = true;
        }
    }
    if (!focused) {
        list->scrollToBottom();
    }

    scheduleMeasurementPass();
    schedulePaintResume(renderId);
    QTimer::singleShot(0, this, &ThreadTimelineController::scheduleViewportCheck);
}

void ThreadTimelineController::schedulePaintResume(quint64 renderId)
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

void ThreadTimelineController::updateGapHeights()
{
    if (!area.ui || !area.ui->listWidget) {
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
        const qint64 estimatedHeight = static_cast<qint64>(std::max(0, count)) * average;
        item->setSizeHint(QSize(0,
            static_cast<int>(std::min<qint64>(estimatedHeight, INT_MAX))));
    }
    list->resizeToBottom();
    lastAppliedGapRowHeight = average;
    schedulePaintResume(renderId);
}

void ThreadTimelineController::scheduleMeasurementPass()
{
    if (!rebuilding) {
        measurementTimer.start();
    }
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
    if (difference * 20 >= baseline) {
        updateGapHeights();
    }
}

} // namespace Mattermost
