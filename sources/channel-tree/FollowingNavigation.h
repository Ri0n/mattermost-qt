#pragma once

#include "backend/FollowingModel.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"

namespace Mattermost {

/**
 * Return true when a server-provided resume target is already at or behind the
 * local viewport high-water mark.
 *
 * The server's unread cursor may lag behind local viewport progress until a
 * channel-view acknowledgement reaches it. Navigation must not use such a
 * stale fallback to move a Following/Attention conversation backwards.
 *
 * An uncached target cannot be ordered without fetching its post metadata, so
 * it is not rejected here. The normal navigation resolver may still load it.
 */
inline bool isStaleConversationResumeTarget(const FollowingModel::Entry& entry,
                                            const BackendChannel& channel,
                                            const QString& postId)
{
    if (!entry.hasLocalProgress() || postId.isEmpty()) {
        return false;
    }

    const BackendPost* candidate = channel.postIdToPost.value(postId, nullptr);
    if (!candidate) {
        return false;
    }

    if (candidate->create_at != entry.readThroughCreateAt) {
        return candidate->create_at < entry.readThroughCreateAt;
    }
    return candidate->id <= entry.readThroughPostId;
}

} // namespace Mattermost
