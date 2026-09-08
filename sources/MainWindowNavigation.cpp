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
    if (!area || &area->getChannel() != channel) {
        return;
    }

    auto ensureThreadArea = [this, area, channel, &rootId]() -> ChatArea* {
        if (rootId.isEmpty()) {
            return nullptr;
        }

        for (ChatArea* existing : area->threadsAreas) {
            if (existing && existing->root_id == rootId) {
                return existing;
            }
        }

        auto* threadArea = new ChatArea(backend, *channel, rootId, area);
        area->threadsAreas.insert(threadArea);
        return threadArea;
    };

    // A thread request without a concrete post target means "open at newest".
    // This is used when a followed thread is already fully read: there is no
    // first unread reply to target, and returning to the root would be the wrong
    // end of the conversation.
    if (postId.isEmpty()) {
        if (ChatArea* threadArea = ensureThreadArea()) {
            threadArea->show();
            threadArea->raise();
            threadArea->activateWindow();
            threadArea->goToNewest();
        }
        return;
    }

    // openStoredChannel() may synchronously activate/init the ChatArea and queue
    // its weak default "show newest" position. Invalidate that intent now,
    // before this function queues the actual semantic navigation work.
    area->preparePostNavigation();

    // A permalink can point directly at a thread reply. Replies deliberately do
    // not have rows in the main channel timeline, so route those links to the
    // thread window instead of searching the channel root timeline.
    if (!rootId.isEmpty()) {
        ChatArea* threadArea = ensureThreadArea();
        if (!threadArea) {
            return;
        }

        // The thread constructor/init path also queues its default newest
        // position. The explicit reply jump has stronger intent.
        threadArea->preparePostNavigation();
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

            bool contextReady = false;
            if (!contextPostIds.isEmpty()) {
                contextReady = areaGuard->ensurePinnedPostVisible(postId, contextPostIds,
                                                                  reachedOldest, reachedNewest);
            } else {
                contextReady = areaGuard->ensurePostVisible(postId);
            }
            if (!contextReady) {
                return;
            }

            areaGuard->lockNavigationToPost(postId, 0);
            areaGuard->goToPost(postId);
        });
}

} // namespace Mattermost
