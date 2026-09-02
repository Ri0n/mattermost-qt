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

bool ChannelTimelineController::ensurePinnedPostVisible(const QString& postId)
{
    if (!active || postId.isEmpty() || !area.ui || !area.ui->listWidget) {
        return false;
    }

    BackendPost* target = area.channel.postIdToPost.value(postId, nullptr);
    if (!target || target->hidden || !target->root_id.isEmpty()) {
        return false;
    }

    QStringList chronologicalRootIds;
    chronologicalRootIds.reserve(static_cast<int>(area.channel.posts.size()));
    for (const BackendPost& post : area.channel.posts) {
        if (!post.hidden && post.root_id.isEmpty()) {
            chronologicalRootIds.push_back(post.id);
        }
    }

    const QStringList contextIds = selectPostWindow(
        chronologicalRootIds, postId, PinnedWindowSize);
    if (contextIds.isEmpty()) {
        return false;
    }

    if (contextNavigationActive && contextNavigationPostId == postId) {
        contextOldestPostId = contextIds.first();
        contextNewestPostId = contextIds.last();
        contextReachedOldest = contextOldestPostId == chronologicalRootIds.first();
        contextReachedNewest = contextNewestPostId == chronologicalRootIds.last();
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
            renderTimeline(postId, stableViewportAnchor());
        }
    } else {
        placeApproximateWindow(contextIds, postId);
    }

    return area.ui->listWidget->findPost(postId) != nullptr;
}

} // namespace Mattermost
