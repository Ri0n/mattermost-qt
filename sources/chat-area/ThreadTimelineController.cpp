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

    // The member initializer runs while ChatArea is still being constructed.
    // Defer all UI access until setupUi(), signal wiring and show() have had a
    // chance to complete.
    QTimer::singleShot(0, this, &ThreadTimelineController::start);
}

void ThreadTimelineController::start()
{
    if (rootId.isEmpty() || !area.ui || !area.ui->listWidget) {
        return;
    }

    BackendPost* root = area.channel.postIdToPost.value(rootId, nullptr);
    expectedPostCount = expectedThreadPostCount(root, area.channel, rootId);

    timeline.reset(expectedPostCount);
    if (root) {
        timeline.placeWindow(0, QStringList {rootId});
        nextLogicalIndex = 1;
    }

    initialPagesRemaining = expectedPostCount > ThreadPageSize * SmallThreadPrefetchPages
        ? LargeThreadPrefetchPages
        : std::min(SmallThreadPrefetchPages,
                   std::max(1, (expectedPostCount + ThreadPageSize - 1) / ThreadPageSize));

    // Rebuild immediately as root + a transparent estimated gap. This removes
    // any accidental subset of cached replies that happened to be present in
    // BackendChannel from unrelated channel/context requests.
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
        // A websocket reply is the new logical end. Keep it visible as a real
        // row even when the middle of the thread is still represented by a gap.
        timeline.placeWindow(expectedPostCount - 1, QStringList {post.id});
        renderTimeline(post.id, false);
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
        updateGapHeights();
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
                // Keep hasNext true so a later user scroll can retry a transient
                // failure instead of permanently truncating the thread.
                return;
            }

            if (page.postIds.isEmpty()) {
                guard->hasNext = false;
                return;
            }

            QStringList pageIds = page.postIds;
            if (guard->nextLogicalIndex > 0 && !pageIds.isEmpty()
                && pageIds.front() == guard->rootId) {
                pageIds.removeFirst();
            }
            if (pageIds.isEmpty()) {
                guard->hasNext = false;
                return;
            }

            const QString newCursor = pageIds.back();
            if (!requestedCursor.isEmpty() && newCursor == requestedCursor) {
                guard->hasNext = false;
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
            // has_next is authoritative on modern servers. The full-page check
            // keeps pagination working against older servers that omit it; at
            // worst an exact multiple causes one harmless empty request.
            guard->hasNext = page.hasNext || responseSize >= ThreadPageSize;

            guard->renderTimeline();

            if (guard->initialPagesRemaining > 0) {
                --guard->initialPagesRemaining;
            }
            if (guard->initialPagesRemaining > 0 && guard->hasNext) {
                QTimer::singleShot(0, guard, &ThreadTimelineController::requestNextPage);
                return;
            }

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
        // Without a useful time range, fall back to sequential paging. It is
        // slower but still deterministic and never guesses an invalid cursor.
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
            guard->renderTimeline(page.postIds.front(), true);
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

void ThreadTimelineController::checkViewport()
{
    if (requestInFlight || !area.ui || !area.ui->listWidget) {
        return;
    }

    PostsListWidget* list = area.ui->listWidget;
    QScrollBar* scrollBar = list->verticalScrollBar();
    const int viewportHeight = list->viewport()->height();

    // Normal wheel/PageDown scrolling prefetches before the loaded prefix runs
    // out, independent of the huge estimated gap below it.
    if (hasNext && nextLogicalIndex < timeline.totalCount()) {
        const qint64 prefixBoundary = timeline.estimatedPixelForIndex(nextLogicalIndex);
        const qint64 viewportBottom = static_cast<qint64>(scrollBar->value()) + viewportHeight;
        if (viewportBottom + static_cast<qint64>(viewportHeight) * 2 >= prefixBoundary
            && viewportBottom <= prefixBoundary + static_cast<qint64>(viewportHeight) * 2) {
            requestNextPage();
            return;
        }
    }

    // A scrollbar drag can land in the middle of a virtual gap. Map that pixel
    // position back to an approximate logical reply index and debounce the
    // server seek until the thumb stops moving briefly.
    QListWidgetItem* gap = gapAtViewportCenter();
    if (!gap) {
        pendingSeekIndex = -1;
        seekTimer.stop();
        return;
    }

    const int targetIndex = logicalIndexInsideGap(gap);
    if (targetIndex < 0 || std::abs(targetIndex - nextLogicalIndex) < ThreadPageSize) {
        return;
    }

    if (scrollBar->isSliderDown()) {
        pendingSeekIndex = targetIndex;
        seekTimer.start();
        return;
    }

    // sliderReleased can be delivered after the last valueChanged.
    requestSeek(targetIndex);
}

void ThreadTimelineController::renderTimeline(const QString& focusPostId, bool focusAtTop)
{
    if (!area.ui || !area.ui->listWidget || rebuilding) {
        return;
    }

    rebuilding = true;
    PostsListWidget* list = area.ui->listWidget;
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
            // Deliberately do not set text, icon, background or itemWidget().
            // It contributes geometry only; the list viewport background shows
            // through the entire unloaded region.
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
            scheduleMeasuredHeightUpdate(postId);
            connect(postWidget, &PostWidget::dimensionsChanged, this,
                    [this, postId] { scheduleMeasuredHeightUpdate(postId); });
        }
    }

    rebuilding = false;

    if (!focusPostId.isEmpty()) {
        const int row = list->findPostByIndex(focusPostId, 0);
        if (row >= 0) {
            list->scrollToItem(list->item(row), focusAtTop
                ? QAbstractItemView::PositionAtTop
                : QAbstractItemView::EnsureVisible);
        }
    }

    QTimer::singleShot(0, this, [this] {
        updateGapHeights();
        scheduleViewportCheck();
    });
}

void ThreadTimelineController::updateGapHeights()
{
    if (!area.ui || !area.ui->listWidget) {
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
        const qint64 estimatedHeight = static_cast<qint64>(std::max(0, count)) * average;
        const qint64 boundedHeight = std::min<qint64>(estimatedHeight, INT_MAX);
        item->setSizeHint(QSize(0, static_cast<int>(boundedHeight)));
    }
}

void ThreadTimelineController::scheduleMeasuredHeightUpdate(const QString& postId)
{
    QTimer::singleShot(0, this, [this, postId] {
        if (!area.ui || !area.ui->listWidget) {
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

QListWidgetItem* ThreadTimelineController::gapAtViewportCenter() const
{
    if (!area.ui || !area.ui->listWidget) {
        return nullptr;
    }
    PostsListWidget* list = area.ui->listWidget;
    QListWidgetItem* item = list->itemAt(list->viewport()->rect().center());
    return PostsListWidget::isGapItem(item) ? item : nullptr;
}

int ThreadTimelineController::logicalIndexInsideGap(const QListWidgetItem* gapItem) const
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

} // namespace Mattermost
