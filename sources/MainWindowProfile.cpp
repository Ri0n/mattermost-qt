#include "mainwindow.h"

#include "backend/Backend.h"
#include "backend/types/BackendUser.h"
#include "info-dialogs/UserProfileDialog.h"

namespace Mattermost {

void MainWindow::on_usericon_label_clicked()
{
    const BackendUser& user = backend.getLoginUser();
    if (user.id.isEmpty()) {
        return;
    }
    UserProfileDialog::showTransient(backend, user, this);
}

} // namespace Mattermost
