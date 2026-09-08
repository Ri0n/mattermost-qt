#include "OutgoingPostCreator.h"

#include <QDebug>
#include <QMessageBox>
#include <QStringList>
#include <QVariant>

#include "backend/PostProps.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendNewPollData.h"
#include "backend/types/BackendPoll.h"
#include "backend/types/BackendPost.h"
#include "backend/types/BackendTeam.h"
#include "channel-tree/ChannelItem.h"
#include "channel-tree/SidebarItem.h"
#include "chat-area/ChatArea.h"

namespace Mattermost {
namespace {

constexpr char PendingPollQuestionProperty[] = "_mmqt_pending_poll_question";
constexpr char PendingPollOptionsProperty[] = "_mmqt_pending_poll_options";
constexpr char PendingPollRootIdProperty[] = "_mmqt_pending_poll_root_id";
constexpr char PendingPollProgressProperty[] = "_mmqt_pending_poll_progress";

void clearPendingPollProperties(OutgoingPostCreator& creator)
{
    creator.setProperty(PendingPollQuestionProperty, QVariant());
    creator.setProperty(PendingPollOptionsProperty, QVariant());
    creator.setProperty(PendingPollRootIdProperty, QVariant());
    creator.setProperty(PendingPollProgressProperty, QVariant());
}

} // namespace

QString OutgoingPostCreator::pollCommandTeamId() const
{
    if (channel && channel->team) {
        return channel->team->id;
    }

    // DM/GM channels do not belong to a team, but Mattermost slash commands
    // still execute in a team context. The sidebar row is the authoritative
    // source for that context. A thread inherits it from its parent ChatArea.
    for (QWidget* widget = parentWidget(); widget; widget = widget->parentWidget()) {
        auto* area = qobject_cast<ChatArea*>(widget);
        if (!area) {
            continue;
        }

        for (ChatArea* contextArea = area; contextArea;
             contextArea = contextArea->parentChatArea()) {
            if (contextArea->channel.team) {
                return contextArea->channel.team->id;
            }
            if (!contextArea->treeItem) {
                continue;
            }

            const QString teamId = contextArea->treeItem
                ->data(0, SidebarItem::TeamIdRole).toString();
            if (!teamId.isEmpty()) {
                return teamId;
            }
        }
        break;
    }

    return QString();
}

void OutgoingPostCreator::createPoll()
{
    if (outgoingPostData) {
        return;
    }

    if (postToEdit) {
        QMessageBox::information(this, tr("Cannot create poll"),
                                 tr("Finish or cancel the current edit before creating a poll."));
        return;
    }

    if (!property(PostProps::ReplyToPostId).toString().isEmpty()) {
        QMessageBox::information(this, tr("Cannot create poll"),
                                 tr("Cancel the quoted reply before creating a poll."));
        return;
    }

    if (!toPlainText().trimmed().isEmpty() || attachmentList) {
        QMessageBox::information(this, tr("Cannot create poll"),
                                 tr("Send or clear the current draft before creating a poll."));
        return;
    }

    // Keep poll creation on the same composer state machine as the existing
    // /poll path. This preserves send/retry/status handling and leaves the
    // command itself as a backwards-compatible keyboard entry point.
    setPlainText(QStringLiteral("/poll"));
    sendPostButtonAction();

    // sendPostButtonAction() only opens the modeless QDialog and returns. The
    // synthetic command is no longer needed; accepted poll data is captured
    // independently by the dialog's accepted signal.
    if (toPlainText() == QStringLiteral("/poll")) {
        clear();
    }
}

void OutgoingPostCreator::armPollRealtimeAcknowledgement(const BackendNewPollData& pollData)
{
    QStringList options;
    options.reserve(pollData.options.size());
    for (const QString& option : pollData.options) {
        options.push_back(option);
    }

    setProperty(PendingPollQuestionProperty, pollData.question);
    setProperty(PendingPollOptionsProperty, options);
    setProperty(PendingPollRootIdProperty, pollData.rootId);
    setProperty(PendingPollProgressProperty, pollData.showProgress);

    if (channel) {
        connect(channel, &BackendChannel::onNewPost,
                this, &OutgoingPostCreator::onPollPostReceived,
                Qt::UniqueConnection);
    }

    qInfo().noquote() << "Poll send armed: channel="
                      << (channel ? channel->id : QString())
                      << "root=" << pollData.rootId
                      << "team=" << pollData.commandTeamId
                      << "options=" << options.size();
}

void OutgoingPostCreator::onPollPostReceived(BackendPost& post)
{
    const QString expectedQuestion = property(PendingPollQuestionProperty).toString();
    if (expectedQuestion.isEmpty()) {
        return;
    }

    // The HTTP acknowledgement may have won the race. Drop the stale realtime
    // correlation key when there is no longer a composer operation to finish.
    if (!isWaitingForPostServerResponse()) {
        clearPendingPollProperties(*this);
        return;
    }

    if (!channel || post.channel_id != channel->id || !post.poll) {
        return;
    }

    const QString expectedRootId = property(PendingPollRootIdProperty).toString();
    if (post.root_id != expectedRootId || post.poll->title != expectedQuestion) {
        return;
    }

    const QStringList expectedOptions = property(PendingPollOptionsProperty).toStringList();
    if (post.poll->options.size() < expectedOptions.size()) {
        return;
    }

    const bool showProgress = property(PendingPollProgressProperty).toBool();
    for (int i = 0; i < expectedOptions.size(); ++i) {
        QString expectedName = expectedOptions.at(i);
        if (showProgress) {
            expectedName += QStringLiteral(" (0)");
        }
        if (post.poll->options.at(i).name != expectedName) {
            return;
        }
    }

    qInfo().noquote() << "Poll send acknowledged by realtime post: post=" << post.id
                      << "poll=" << post.poll->id;
    clearPendingPollProperties(*this);
    finishSend(post.id);
}

} // namespace Mattermost
