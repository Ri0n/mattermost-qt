#include "mainwindow.h"

#include "backend/Backend.h"
#include "backend/types/BackendChannel.h"
#include "chat-area/ChatArea.h"
#include "ui_mainwindow.h"

namespace Mattermost {

void MainWindow::openChannelPost(const QString& channelId, const QString& postId)
{
    if (channelId.isEmpty()) {
        return;
    }

    BackendChannel* channel = backend.getStorage().getChannelById(channelId);
    if (!channel) {
        return;
    }

    ui->channelList->openChannel(channelId);
    ChatArea* area = ui->channelList->getCurrentPage();
    if (!area || &area->getChannel() != channel || postId.isEmpty()) {
        return;
    }

    // PostNavigationService has already cached context around permalink targets.
    // Make that semantic target authoritative while sparse rows materialize so
    // ordinary bottom-follow/prefetch cannot steal the viewport.
    area->lockNavigationToPost(postId, 0);
    area->ensurePostVisible(postId);
    area->goToPost(postId);
}

} // namespace Mattermost
