#include "OutgoingPostCreator.h"

#include <QMessageBox>

#include "backend/PostProps.h"

namespace Mattermost {

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

    // /poll opens the modal dialog synchronously and returns without consuming
    // the editor. Do not leave the synthetic command behind when the dialog is
    // cancelled; accepted poll data is already captured independently.
    if (toPlainText() == QStringLiteral("/poll")) {
        clear();
    }
}

} // namespace Mattermost
