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
        // Keep the same semantic lock used by channel permalink navigation: an
        // overlapping async thread page or attachment reflow must not move the
        // unread/permalink target before the user scrolls.
        QPointer<ChatArea> threadGuard(threadArea);
        QTimer::singleShot(0, threadArea, [threadGuard, postId] {
            if (!threadGuard) {
                return;
            }
            threadGuard->lockNavigationToPost(postId, 0);
            threadGuard->ensurePostVisible(postId);
            threadGuard->goToPost(postId);
        });
        return;
    }

    // A freshly opened lazy ChatArea also starts its sparse controller on the
    // next event-loop turn. Apply the already-fetched permalink context after
    // that start so the target cannot be replaced by initial timeline setup.
    QPointer<ChatArea> areaGuard(area);
    QTimer::singleShot(0, area,
        [areaGuard, postId, contextPostIds, reachedOldest, reachedNewest] {
            if (!areaGuard) {
                return;
            }

            areaGuard->lockNavigationToPost(postId, 0);
            if (!contextPostIds.isEmpty()) {
                areaGuard->ensurePinnedPostVisible(postId, contextPostIds,
                                                   reachedOldest, reachedNewest);
            } else {
                areaGuard->ensurePostVisible(postId);
            }
            areaGuard->goToPost(postId);
        });
}

} // namespace Mattermost
