#include "ChatArea.h"

#include <QTimer>

#include "backend/Backend.h"
#include "backend/SidebarService.h"
#include "backend/types/BackendChannel.h"
#include "PostsListWidget.h"
#include "ui_ChatArea.h"

namespace Mattermost {
namespace {

bool hasLoadedLatestPost(const BackendChannel& channel)
{
    if (channel.last_post_at == 0) {
        return true;
    }

    for (const BackendPost& post : channel.posts) {
        if (post.create_at >= channel.last_post_at) {
            return true;
        }
    }
    return false;
}

bool hasRenderedPost(const PostsListWidget* list)
{
    if (!list) {
        return false;
    }

    for (int row = 0; row < list->count(); ++row) {
        if (PostsListWidget::isPostItem(list->item(row))) {
            return true;
        }
    }
    return false;
}

} // namespace

void ChatArea::requestExplicitReadAcknowledgement()
{
    if (isThread) {
        return;
    }

    explicitReadPending = true;
    QObject::disconnect(explicitReadPostsConnection);
    explicitReadPostsConnection = connect(
        &channel, &BackendChannel::onNewPosts, this,
        [this](const ChannelNewPosts&) {
            // fillChannelPosts() is connected to the same signal. Defer one
            // event-loop turn so the post widgets and final scroll position are
            // established before we acknowledge the explicit navigation.
            QTimer::singleShot(0, this, &ChatArea::tryExplicitReadAcknowledgement);
        });

    // Already-loaded channels do not emit onNewPosts when merely reselected.
    QTimer::singleShot(0, this, &ChatArea::tryExplicitReadAcknowledgement);
}

void ChatArea::tryExplicitReadAcknowledgement()
{
    if (!explicitReadPending || isThread) {
        return;
    }

    // If the user left before the slow request completed, the original click
    // must not consume unread state in the background.
    if (backend.getCurrentChannel() != &channel) {
        explicitReadPending = false;
        QObject::disconnect(explicitReadPostsConnection);
        explicitReadPostsConnection = QMetaObject::Connection();
        return;
    }

    if (!initialized || !hasLoadedLatestPost(channel) || !hasRenderedPost(ui->listWidget)) {
        return;
    }

    auto& sidebar = SidebarService::instance(backend);
    if (!sidebar.isChannelUnread(channel) && !sidebar.hasUnreadMention(channel.id)) {
        explicitReadPending = false;
        QObject::disconnect(explicitReadPostsConnection);
        explicitReadPostsConnection = QMetaObject::Connection();
        return;
    }

    explicitReadPending = false;
    QObject::disconnect(explicitReadPostsConnection);
    explicitReadPostsConnection = QMetaObject::Connection();

    setUnreadMessagesCount(0);
    sidebar.markChannelViewedLocally(channel);
    backend.markChannelAsViewed(channel);
}

} // namespace Mattermost
