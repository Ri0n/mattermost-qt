#include "ChatArea.h"

#include "backend/types/BackendPost.h"
#include "ui_ChatArea.h"

namespace Mattermost {

void ChatArea::editPost(BackendPost& post)
{
    if (!ui || !ui->outgoingPostCreator) {
        return;
    }
    ui->outgoingPostCreator->postEditInitiated(post);
    ui->outgoingPostCreator->setFocus(Qt::OtherFocusReason);
}

} // namespace Mattermost
