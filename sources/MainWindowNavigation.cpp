#include "mainwindow.h"

#include <QPointer>
#include <QTimer>

#include "backend/Backend.h"
#include "backend/types/BackendChannel.h"
#include "channel-tree/ChannelTree.h"
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

    ui->channelList->openStoredChannel(channelId);
    ChatArea* area = ui->channelList->getCurrentPage();
    if (!area || &area->getChannel() != channel || postId.isEmpty()) {
        return;
    }

    // A permalink can point directly at a thread reply. Replies deliberately do
    // not have rows in the main channel timeline, so route those links to the
    // thread window instead of searching the channel root timeline.
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

        // A newly created thread ChatArea installs its ThreadPostSource on the
        // next event-loop turn. Queue the semantic target behind that setup so
        // even a reply outside the initial thread page can be materialized from
        // the cached post. Keep the semantic viewport lock used by channel
        // permalink navigation so an overlapping async page or attachment reflow
        // cannot move the target before the user scrolls.
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

    // A freshly opened lazy ChatArea installs its ChannelPostSource on the next
    // event-loop turn. Apply the already-fetched permalink context after that
    // setup. The context must be published before the viewport is moved: an
    // isolated estimated target is deliberately no longer a valid source row.
    QPointer<ChatArea> areaGuard(area);
    QTimer::singleShot(0, area,
        [areaGuard, postId, contextPostIds, reachedOldest, reachedNewest] {
            if (!areaGuard) {
                return;
            }

            if (!contextPostIds.isEmpty()) {
                if (!areaGuard->ensurePinnedPostVisible(postId, contextPostIds,
                                                        reachedOldest, reachedNewest)) {
                    return;
                }
            } else if (!areaGuard->ensurePostVisible(postId)) {
                return;
            }

            areaGuard->lockNavigationToPost(postId, 0);
            areaGuard->goToPost(postId);
        });
}

} // namespace Mattermost
