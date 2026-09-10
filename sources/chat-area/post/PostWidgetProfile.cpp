#include "PostWidget.h"

#include "backend/types/BackendUser.h"
#include "info-dialogs/UserProfileDialog.h"

namespace Mattermost {

void PostWidget::on_authorAvatar_clicked()
{
    if (!post.author) {
        return;
    }
    UserProfileDialog::showTransient(backend_, *post.author, this);
}

} // namespace Mattermost
