#include "BackendChannel.h"

#include "backend/LocalPostDeleteTracker.h"
#include "log.h"

namespace Mattermost {

void BackendChannel::deletePost(QString postId)
{
    const bool locallyInitiated = LocalPostDeleteTracker::take(postId);
    BackendPost* existingPost = findPostById(postId);
    if (!existingPost) {
        LOG_DEBUG("BackendChannel::deletePost: post with ID " << postId << " not found");
        // Consumers may still know the semantic ID even when the post is not
        // currently materialized, so preserve the notification.
        emit onPostDeleted(postId);
        return;
    }

    // Match the official Mattermost distinction between the client that issued
    // DELETE /posts/{id} and other clients receiving post_deleted remotely. For
    // a locally initiated top-level delete, emit while isOwnPost() is still true;
    // ChannelPostSource then removes the logical row completely. A remote
    // websocket deletion is marked deleted first and therefore renders as a
    // tombstone, even when the original author is the current user.
    if (locallyInitiated
        && existingPost->isOwnPost()
        && existingPost->root_id.isEmpty()
        && !existingPost->hidden) {
        emit onPostDeleted(postId);
        existingPost->isDeleted = true;
        return;
    }

    if (!existingPost->isDeleted) {
        existingPost->isDeleted = true;

        // Keep the visible thread summary consistent when a cached reply is
        // deleted. last_reply_at cannot be reconstructed cheaply here and will
        // be refreshed by the next authoritative thread response.
        if (!existingPost->root_id.isEmpty()) {
            if (BackendPost* rootPost = findPostById(existingPost->root_id)) {
                if (rootPost->reply_count > 0) {
                    --rootPost->reply_count;
                }
                emit onPostEdited(*rootPost);
            }
        }
    }

    emit onPostDeleted(postId);
}

} // namespace Mattermost
