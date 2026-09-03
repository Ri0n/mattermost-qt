#include "mainwindow.h"

#include <QPointer>
#include <QTimer>

#include "backend/Backend.h"
#include "backend/types/BackendChannel.h"
#include "chat-area/ChatArea.h"
#include "ui_mainwindow.h"

namespace Mattermost {

void MainWindow::openChannelPost(const QString& channelId,
                                 const QString& postId,
                                 const QString& rootId,
                                 const QStringList& contextPostIds,
                                 bool reachedOldest,
                                 bool reachedNewest)
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

    // A permalink can point directly at a thread reply. Replies deliberately do
    // not have rows in the main channel timeline, so route those links to the
    // thread window instead of repeatedly searching the channel QListWidget.
    if (!rootId.isEmpty()) {
        ChatArea* threadArea = nullptr;
        for (ChatArea* existing : area->threadsAreas) {
            if (existing && existing->root_id == rootId) {
                threadArea = existing;
                break;
            }
        }

        if (!threadArea) {
            threadArea = new ChatArea(backend, *channel, rootId, area);
            area->threadsAreas.insert(threadArea);
        }

        threadArea->show();
        threadArea->raise();
        threadArea->activateWindow();

        // ThreadTimelineController starts on the next event-loop turn. Queue the
        // semantic target behind that start so even a reply outside the initial
        // 30-row thread page can be materialized immediately from the cached post.
        QPointer<ChatArea> threadGuard(threadArea);
        QTimer::singleShot(0, threadArea, [threadGuard, postId] {
            if (!threadGuard) {
                return;
            }
            threadGuard->ensurePostVisible(postId);
            threadGuard->goToPost(postId);
        });
        return;
    }

    // PostNavigationService has already cached a bounded server context around
    // the permalink target. Feed that exact context to the sparse controller;
    // guessing a single row's position inside a large logical gap is unreliable.
    area->lockNavigationToPost(postId, 0);
    if (!contextPostIds.isEmpty()) {
        area->ensurePostContextVisible(postId, contextPostIds,
                                       reachedOldest, reachedNewest);
    } else {
        area->ensurePostVisible(postId);
    }
    area->goToPost(postId);
}

} // namespace Mattermost
