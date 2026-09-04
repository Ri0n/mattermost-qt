#include "ChannelTimelineController.h"

#include <algorithm>

#include "ChatArea.h"
#include "PostsListWidget.h"
#include "backend/PostWindowSelection.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "ui_ChatArea.h"

namespace Mattermost {
namespace {

constexpr int PinnedWindowSize = 31;

} // namespace

bool ChannelTimelineController::ensurePinnedPostVisible(const QString& postId,
                                                        const QStringList& contextPostIds,
                                                        bool reachedOldest,
                                                        bool reachedNewest)
{
    if (!active || postId.isEmpty() || !area.ui || !area.ui->listWidget) {
        return false;
    }

    BackendPost* target = area.channel.postIdToPost.value(postId, nullptr);
    if (!target || target->hidden || !target->root_id.isEmpty()) {
        return false;
    }

    const QStringList contextIds = selectPostWindow(
        contextPostIds, postId, PinnedWindowSize);
    if (contextIds.isEmpty()) {
        return false;
    }

    if (contextNavigationActive && contextNavigationPostId == postId) {
        contextOldestPostId = contextIds.first();
        contextNewestPostId = contextIds.last();
        // A server edge is authoritative only when the selected 31-row window
        // actually includes that edge of the request-local reserve.
        contextReachedOldest = reachedOldest
            && contextOldestPostId == contextPostIds.first();
        contextReachedNewest = reachedNewest
            && contextNewestPostId == contextPostIds.last();
    }

    const int existingTargetIndex = timeline.indexOf(postId);
    if (existingTargetIndex >= 0) {
        const int targetOffset = contextIds.indexOf(postId);
        int firstIndex = existingTargetIndex - targetOffset;

        if (firstIndex < 0 && !totalCountExact) {
            const int growBy = -firstIndex;
            timeline.setTotalCountPreservingNewest(timeline.totalCount() + growBy);
            expectedPostCount = timeline.totalCount();
            firstIndex = 0;
        }

        const int requiredCount = firstIndex + static_cast<int>(contextIds.size());
        if (requiredCount > timeline.totalCount() && !totalCountExact) {
            timeline.setTotalCount(requiredCount);
            expectedPostCount = requiredCount;
        }

        if (firstIndex >= 0 && firstIndex < timeline.totalCount()) {
            timeline.placeWindow(firstIndex, contextIds);
        }
    } else {
        // Defer the render until confirmed server boundaries have been applied;
        // otherwise a pinned post near the beginning briefly shows an obsolete
        // leading gap and then jumps again when the boundary is corrected.
        placeApproximateWindow(contextIds, postId, false);
    }

    if (contextNavigationActive && contextNavigationPostId == postId) {
        if (contextReachedOldest && !contextOldestPostId.isEmpty()) {
            timeline.alignLoadedSpanToBoundary(contextOldestPostId, true);
        }
        if (contextReachedNewest && !contextNewestPostId.isEmpty()) {
            timeline.alignLoadedSpanToBoundary(contextNewestPostId, false);
        }
    }

    if (timeline.contains(postId)) {
        renderTimeline(postId, stableViewportAnchor());
    }
    return area.ui->listWidget->findPost(postId) != nullptr;
}

} // namespace Mattermost
