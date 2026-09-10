#include "ChatArea.h"

#include <QCursor>

#include "backend/Backend.h"
#include "backend/Storage.h"
#include "backend/types/BackendUser.h"
#include "info-dialogs/UserProfileDialog.h"
#include "ui_ChatArea.h"

namespace Mattermost {

void ChatArea::on_userAvatar_clicked()
{
    if (isThread) {
        return;
    }

    const BackendUser* user = backend.getStorage().getUserById(channel.name);
    if (!user) {
        return;
    }

    UserProfileDialog::showTransient(backend, *user, this);
}

} // namespace Mattermost
