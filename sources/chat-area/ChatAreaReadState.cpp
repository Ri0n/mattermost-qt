#include "ChatArea.h"

#include <QPointer>
#include <QTimer>

#include "AbstractPostSource.h"
#include "ChatLogWidget.h"
#include "backend/Backend.h"
#include "backend/FollowingModel.h"
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

void ChatArea::requestThreadReadAcknowledgement()
{
    if (!isThread || root_id.isEmpty() || !ui || !ui->listWidget) {
        return;
    }

    // A live reply can arrive before LongListWidget has materialized its new
    // logical tail. Remember the semantic read intent and let the normal view
    // synchronization finish first; no pixel/layout event is itself a read.
    threadReadPending = true;
    QTimer::singleShot(0, this, &ChatArea::tryThreadReadAcknowledgement);
}

void ChatArea::tryThreadReadAcknowledgement()
{
    if (!threadReadPending || threadReadInFlight || !isThread || !initialized
        || root_id.isEmpty() || !channel.team || !ui || !ui->listWidget) {
        return;
    }

    // Thread read state follows what was actually presented to the user. Merely
    // keeping a thread object alive in the background must not consume unread
    // replies. Composer focus is deliberately irrelevant: a visible active
    // thread at its newest edge is sufficient.
    if (!isVisible() || !isActiveWindow() || !ui->listWidget->isAtEnd()
        || !hasRenderedNewestPost(ui->listWidget)) {
        return;
    }

    threadReadPending = false;
    threadReadInFlight = true;

    const QString teamId = channel.team->id;
    const QString threadId = root_id;
    QPointer<ChatArea> guard(this);
    FollowingModel::instance(backend).markThreadRead(
        teamId, threadId,
        [guard](bool) {
            if (!guard) {
                return;
            }

            guard->threadReadInFlight = false;
            // Several replies may have arrived while the first read request was
            // in flight. Coalesce that burst into one more read at the newest
            // currently presented edge instead of issuing one request per post.
            if (guard->threadReadPending) {
                QTimer::singleShot(0, guard,
                                   &ChatArea::tryThreadReadAcknowledgement);
            }
        });
}

} // namespace Mattermost
