#include "ChatArea.h"

#include <QTimer>

#include "AbstractPostSource.h"
#include "ChatLogWidget.h"
#include "backend/Backend.h"
#include "backend/SidebarService.h"
#include "backend/types/BackendChannel.h"
#include "ui_ChatArea.h"

namespace Mattermost {
namespace {

bool hasRenderedNewestPost(const ChatLogWidget* list)
{
    if (!list || !list->source()) {
        return false;
    }

    const AbstractPostSource* source = list->source();
    const int count = source->itemCount();
    if (count <= 0) {
        return true;
    }

    const int newest = count - 1;
    return source->isAvailable(newest) && list->itemWidget(newest) != nullptr;
}

} // namespace

void ChatArea::requestExplicitReadAcknowledgement()
{
    if (isThread || !ui || !ui->listWidget) {
        return;
    }

    explicitReadPending = true;
    QObject::disconnect(explicitReadPostsConnection);

    // The source/network layer is intentionally independent from the old
    // BackendChannel::onNewPosts rendering signal. A read acknowledgement waits
    // for the logical newest row to become a concrete PostWidget instead.
    explicitReadPostsConnection = connect(
        ui->listWidget, &LongListWidget::materializedRangeChanged, this,
        [this](int, int) {
            QTimer::singleShot(0, this, &ChatArea::tryExplicitReadAcknowledgement);
        });

    // Already-materialized channels need no further range signal.
    QTimer::singleShot(0, this, &ChatArea::tryExplicitReadAcknowledgement);
}

void ChatArea::tryExplicitReadAcknowledgement()
{
    if (!explicitReadPending || isThread) {
        return;
    }

    // If the user left before the async range request completed, the original
    // click must not consume unread state in the background.
    if (backend.getCurrentChannel() != &channel) {
        explicitReadPending = false;
        QObject::disconnect(explicitReadPostsConnection);
        explicitReadPostsConnection = QMetaObject::Connection();
        return;
    }

    if (!initialized || !hasRenderedNewestPost(ui->listWidget)) {
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
