#include "BackendChannel.h"

#include "log.h"

namespace Mattermost {

void BackendChannel::deletePost(QString postId)
{
    BackendPost* existingPost = findPostById(postId);
    if (!existingPost) {
        LOG_DEBUG("BackendChannel::deletePost: post with ID " << postId << " not found");
        // Consumers may still know the semantic ID even when the post is not
        // currently materialized, so preserve the notification.
        emit onPostDeleted(postId);
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
