#include "mainwindow.h"

#include <QLoggingCategory>
#include <QPointer>
#include <QTimer>

#include "backend/Backend.h"
#include "backend/types/BackendChannel.h"
#include "channel-tree/ChannelTree.h"
#include "chat-area/ChatArea.h"
#include "navigation/NavigationUiController.h"
#include "ui_mainwindow.h"

namespace Mattermost {
namespace {

Q_LOGGING_CATEGORY(lcNavigationJump, "mattermost.navigation.jump", QtWarningMsg)

} // namespace

void MainWindow::openChannelPost(const QString& channelId,
                                 const QString& postId,
                                 const QString& rootId,
                                 const QStringList& contextPostIds,
                                 bool reachedOldest,
                                 bool reachedNewest,
                                 bool preserveIfOpen)
{
    if (channelId.isEmpty()) {
        return;
    }

    BackendChannel* channel = backend.getStorage().getChannelById(channelId);
    if (!channel) {
        return;
    }

    if (!postId.isEmpty()) {
        QStringList indexedContext;
        indexedContext.reserve(contextPostIds.size());
        for (int index = 0; index < contextPostIds.size(); ++index) {
            indexedContext.push_back(QString::number(index) + QLatin1Char(':')
                                     + contextPostIds.at(index));
        }
        qCDebug(lcNavigationJump).nospace()
            << "JUMP_OPEN channel=" << channelId
            << " post=" << postId
            << " root=" << rootId
            << " contextCount=" << contextPostIds.size()
            << " reachedOldest=" << reachedOldest
            << " reachedNewest=" << reachedNewest
            << " preserveIfOpen=" << preserveIfOpen
            << " context=[" << indexedContext.join(QLatin1Char(',')) << ']';
    }

    auto& navigationUi = NavigationUiController::instance(*this);

    // Thread presentation is independent from the main-channel surface. A
    // followed thread can therefore open in the right pane without stealing an
    // already visible central channel. A completely empty centre is different:
    // showing a thread beside blank space looks broken, so establish its parent
    // channel as the central context first.
    if (!rootId.isEmpty()) {
        ChatArea* centralArea = ui->channelList->getCurrentPage();
        if (!centralArea) {
            ui->channelList->openStoredChannel(channelId);
            centralArea = ui->channelList->getCurrentPage();
            if (!centralArea) {
                return;
            }

            // ChannelTree records the newly activated main-channel location on
            // the next event-loop turn. Let that settle before presenting the
            // thread, otherwise its delayed history update can arrive after the
            // thread record and make the parent channel look like the active
            // semantic destination.
            QPointer<MainWindow> guard(this);
            QTimer::singleShot(0, this,
                [guard, channelId, postId, rootId, contextPostIds,
                 reachedOldest, reachedNewest, preserveIfOpen] {
                    if (guard) {
                        guard->openChannelPost(channelId, postId, rootId,
                                               contextPostIds, reachedOldest,
                                               reachedNewest, preserveIfOpen);
                    }
                });
            return;
        }

        ChatArea* threadArea = navigationUi.findThread(channelId, rootId);
        if (threadArea && preserveIfOpen) {
            navigationUi.presentThread(threadArea);
            return;
        }

        const bool created = !threadArea;
        if (!threadArea) {
            ChatArea* parentArea = centralArea;
            if (!parentArea || &parentArea->getChannel() != channel) {
                parentArea = nullptr;
            }

            threadArea = new ChatArea(backend, *channel, rootId, parentArea);
            if (parentArea) {
                parentArea->threadsAreas.insert(threadArea);
            }
        }

        if (postId.isEmpty()) {
            navigationUi.presentThread(threadArea);
            if (created || !preserveIfOpen) {
                threadArea->goToNewest();
            }
            return;
        }

        // A permalink/notification is an explicit semantic target and may
        // reposition an existing thread. Following uses preserveIfOpen and was
        // handled above, so its repeat activation never disturbs the viewport.
        threadArea->preparePostNavigation();
        navigationUi.presentThread(threadArea);

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

    ui->channelList->openStoredChannel(channelId);
    ChatArea* area = ui->channelList->getCurrentPage();
    if (!area || &area->getChannel() != channel) {
        return;
    }

    if (postId.isEmpty()) {
        return;
    }

    // openStoredChannel() may synchronously activate/init the ChatArea and queue
    // its weak default "show newest" position. Invalidate that intent now,
    // before this function queues the actual semantic navigation work.
    area->preparePostNavigation();

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
            qCDebug(lcNavigationJump).nospace()
                << "JUMP_APPLY post=" << postId
                << " contextReady=" << contextReady;
            if (!contextReady) {
                return;
            }

            areaGuard->lockNavigationToPost(postId, 0);
            areaGuard->goToPost(postId);
        });
}

} // namespace Mattermost
