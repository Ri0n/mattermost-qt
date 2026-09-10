#include "mainwindow.h"

#include <memory>

#include <QLoggingCategory>
#include <QMetaObject>
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

    // openStoredChannel() may have to join a public channel asynchronously.
    // Preserve the complete semantic navigation request so the eventual channel
    // admission resumes the original permalink/thread jump rather than merely
    // leaving the newly joined channel open at its default position.
    const auto armStoredChannelRetry = [this, channelId, postId, rootId,
                                        contextPostIds, reachedOldest,
                                        reachedNewest, preserveIfOpen] {
        auto connection = std::make_shared<QMetaObject::Connection>();
        QPointer<MainWindow> guard(this);
        *connection = connect(ui->channelList, &ChannelTree::storedChannelOpenFinished,
                              this,
                              [guard, connection, channelId, postId, rootId,
                               contextPostIds, reachedOldest, reachedNewest,
                               preserveIfOpen](const QString& completedChannelId,
                                              bool opened) {
            if (completedChannelId != channelId) {
                return;
            }
            QObject::disconnect(*connection);
            if (!guard || !opened) {
                return;
            }

            QTimer::singleShot(0, guard.data(),
                [guard, channelId, postId, rootId, contextPostIds,
                 reachedOldest, reachedNewest, preserveIfOpen] {
                    if (guard) {
                        guard->openChannelPost(channelId, postId, rootId,
                                               contextPostIds, reachedOldest,
                                               reachedNewest, preserveIfOpen);
                    }
                });
        });
        return connection;
    };

    // Thread presentation is independent from the main-channel surface. A
    // followed thread can therefore open in the right pane without stealing an
    // already visible central channel. A completely empty centre is different:
    // showing a thread beside blank space looks broken, so establish its parent
    // channel as the central context first.
    if (!rootId.isEmpty()) {
        ChatArea* centralArea = ui->channelList->getCurrentPage();
        if (!centralArea) {
            const auto retryConnection = armStoredChannelRetry();
            ui->channelList->openStoredChannel(channelId);
            centralArea = ui->channelList->getCurrentPage();
            if (!centralArea || &centralArea->getChannel() != channel) {
                return;
            }
            QObject::disconnect(*retryConnection);

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
            threadArea->requestExplicitReadAcknowledgement();
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
            threadArea->requestExplicitReadAcknowledgement();
            return;
        }

        // A permalink/notification/queue activation is an explicit semantic
        // target and may reposition an existing thread. Callers that truly want
        // presentation-only behaviour can still opt into preserveIfOpen above.
        threadArea->preparePostNavigation();
        navigationUi.presentThread(threadArea);

        QPointer<ChatArea> threadGuard(threadArea);
        QTimer::singleShot(0, threadArea,
            [threadGuard, postId] {
                if (!threadGuard || !threadGuard->lockNavigationToPost(postId, 0)) {
                    return;
                }
                threadGuard->highlightPostWhenAuthoritative(postId);
            });
        return;
    }

    const auto retryConnection = armStoredChannelRetry();
    ui->channelList->openStoredChannel(channelId);
    ChatArea* area = ui->channelList->getCurrentPage();
    if (!area || &area->getChannel() != channel) {
        return;
    }
    QObject::disconnect(*retryConnection);

    if (postId.isEmpty()) {
        // Re-evaluate the already visible viewport for repeated Following/
        // Attention activation. This does not mark the channel by navigation;
        // ChatLogWidget still requires a concrete post lower edge in view.
        area->requestExplicitReadAcknowledgement();
        return;
    }

    // openStoredChannel() may synchronously activate/init the ChatArea and queue
    // its weak default "show newest" position. Invalidate that intent now,
    // before this function queues the actual semantic navigation work.
    area->preparePostNavigation();

    // A freshly opened lazy ChatArea installs its ChannelPostSource on the next
    // event-loop turn. Publish the already-fetched permalink context before
    // asking ChatLogWidget to establish the one semantic viewport lock.
    QPointer<ChatArea> areaGuard(area);
    QTimer::singleShot(0, area,
        [areaGuard, postId, contextPostIds, reachedOldest, reachedNewest] {
            if (!areaGuard) {
                return;
            }

            if (!contextPostIds.isEmpty()
                && !areaGuard->ensurePinnedPostVisible(postId, contextPostIds,
                                                       reachedOldest, reachedNewest)) {
                qCDebug(lcNavigationJump).nospace()
                    << "JUMP_APPLY post=" << postId << " contextReady=false";
                return;
            }

            const bool navigationReady = areaGuard->lockNavigationToPost(postId, 0);
            qCDebug(lcNavigationJump).nospace()
                << "JUMP_APPLY post=" << postId
                << " contextReady=" << navigationReady;
            if (!navigationReady) {
                return;
            }
            areaGuard->highlightPostWhenAuthoritative(postId);
        });
}

} // namespace Mattermost
