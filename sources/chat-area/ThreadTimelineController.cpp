#include "ThreadTimelineController.h"

#include <algorithm>

#include <QScrollBar>
#include <QTimer>

#include "ChatArea.h"
#include "PostsListWidget.h"
#include "backend/PostTimelineService.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "ui_ChatArea.h"

namespace Mattermost {
namespace {

constexpr int ThreadPageSize = 80;
constexpr int SmallThreadPrefetchPages = 2;
constexpr int LargeThreadPrefetchPages = 3;

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
    if (root) {
        expectedPostCount = std::max(1, static_cast<int>(root->reply_count + 1));
    } else {
        expectedPostCount = std::max(1, countCachedThreadPosts(area.channel, rootId));
    }
    timeline.reset(expectedPostCount);

    initialPagesRemaining = expectedPostCount > ThreadPageSize * SmallThreadPrefetchPages
        ? LargeThreadPrefetchPages
        : std::min(SmallThreadPrefetchPages,
                   std::max(1, (expectedPostCount + ThreadPageSize - 1) / ThreadPageSize));

    // Establish the root/current viewport as an anchor before asynchronous
    // pages start changing list geometry. fillChannelPosts() may ask to scroll
    // to bottom, but PostsListWidget will then restore this concrete anchor.
    area.ui->listWidget->commitCurrentViewportAsAnchor();

    QScrollBar* scrollBar = area.ui->listWidget->verticalScrollBar();
    connect(scrollBar, &QScrollBar::valueChanged, this,
            [this](int) { scheduleViewportCheck(); });

    connect(&area.channel, &BackendChannel::onNewPost, this,
            [this](BackendPost& post) {
        if (post.root_id == rootId) {
            ++expectedPostCount;
            timeline.setTotalCount(expectedPostCount);
        }
    });
    connect(&area.channel, &BackendChannel::onPostEdited, this,
            [this](BackendPost& post) {
        if (post.id == rootId) {
            expectedPostCount = std::max(
                expectedPostCount, static_cast<int>(post.reply_count + 1));
            timeline.setTotalCount(expectedPostCount);
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
                // Keep hasNext true so a later user scroll can retry a transient
                // failure instead of permanently truncating the thread.
                return;
            }

            if (page.postIds.isEmpty()) {
                guard->hasNext = false;
                return;
            }

            const QString newCursor = page.postIds.back();
            if (!requestedCursor.isEmpty() && newCursor == requestedCursor) {
                guard->hasNext = false;
                return;
            }

            const int neededCount = guard->nextLogicalIndex + page.postIds.size();
            if (neededCount > guard->timeline.totalCount()) {
                guard->expectedPostCount = std::max(guard->expectedPostCount, neededCount);
                guard->timeline.setTotalCount(guard->expectedPostCount);
            }
            guard->timeline.placeWindow(guard->nextLogicalIndex, page.postIds);
            guard->nextLogicalIndex += page.postIds.size();

            guard->cursorPostId = newCursor;
            if (BackendPost* cursorPost = guard->area.channel.postIdToPost.value(newCursor, nullptr)) {
                guard->cursorCreateAt = cursorPost->create_at;
            }

            // has_next is authoritative on modern servers. The full-page check
            // keeps pagination working against older servers that omit it; at
            // worst an exact multiple causes one harmless empty request.
            guard->hasNext = page.hasNext || page.postIds.size() >= ThreadPageSize;

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

void ThreadTimelineController::scheduleViewportCheck()
{
    if (viewportCheckScheduled) {
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
    if (requestInFlight || !hasNext || !area.ui || !area.ui->listWidget) {
        return;
    }

    QScrollBar* scrollBar = area.ui->listWidget->verticalScrollBar();
    const int remaining = scrollBar->maximum() - scrollBar->value();
    const int prefetchDistance = std::max(area.ui->listWidget->viewport()->height() * 2,
                                          scrollBar->pageStep() * 2);
    if (remaining <= prefetchDistance) {
        requestNextPage();
    }
}

} // namespace Mattermost
