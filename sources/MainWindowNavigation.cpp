#include "mainwindow.h"

#include <QLoggingCategory>
#include <QPointer>
#include <QScrollBar>
#include <QTimer>

#include "backend/Backend.h"
#include "backend/types/BackendChannel.h"
#include "channel-tree/ChannelTree.h"
#include "chat-area/AbstractPostSource.h"
#include "chat-area/ChatArea.h"
#include "chat-area/ChatLogWidget.h"
#include "ui_ChatArea.h"
#include "ui_mainwindow.h"

namespace Mattermost {

namespace {

Q_LOGGING_CATEGORY(lcJumpTrace, "mattermost.navigation.jump", QtWarningMsg)

void logJumpState(const char* phase, ChatArea* area, const QString& postId)
{
    if (!area || !area->getUi() || !area->getUi()->listWidget) {
        qCWarning(lcJumpTrace).nospace()
            << "STATE phase=" << phase
            << " postId=" << postId
            << " area=null";
        return;
    }

    ChatLogWidget* list = area->getUi()->listWidget;
    AbstractPostSource* source = list->source();
    const int index = source ? source->indexOfPost(postId) : -1;
    QWidget* widget = index >= 0 ? list->itemWidget(index) : nullptr;
    const LongListWidget::Range visible = list->visibleRange();
    QScrollBar* bar = list->verticalScrollBar();

    qCWarning(lcJumpTrace).nospace()
        << "STATE phase=" << phase
        << " postId=" << postId
        << " index=" << index
        << " itemCount=" << list->itemCount()
        << " available=" << (source && index >= 0 ? source->isAvailable(index) : false)
        << " widget=" << static_cast<const void*>(widget)
        << " y=" << (widget ? widget->y() : -1)
        << " h=" << (widget ? widget->height() : -1)
        << " viewportH=" << list->viewport()->height()
        << " visible=[" << visible.first << ',' << visible.last << ']'
        << " scroll=" << bar->value() << '/' << bar->maximum()
        << " atEnd=" << list->isAtEnd()
        << " viewportLock=" << list->hasViewportLock();
}

void scheduleJumpState(QPointer<ChatArea> area,
                       const QString& postId,
                       int delayMs,
                       const char* phase)
{
    if (!area) {
        return;
    }
    QTimer::singleShot(delayMs, area, [area, postId, phase] {
        if (area) {
            logJumpState(phase, area, postId);
        }
    });
}

} // namespace

void MainWindow::openChannelPost(const QString& channelId,
                                 const QString& postId,
                                 const QString& rootId,
                                 const QStringList& contextPostIds,
                                 bool reachedOldest,
                                 bool reachedNewest)
{
    qCWarning(lcJumpTrace).nospace()
        << "REQUEST channelId=" << channelId
        << " postId=" << postId
        << " rootId=" << rootId
        << " contextCount=" << contextPostIds.size()
        << " reachedOldest=" << reachedOldest
        << " reachedNewest=" << reachedNewest;

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
    logJumpState("after-open-channel", area, postId);

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
            logJumpState("thread-before-lock", threadGuard, postId);
            threadGuard->lockNavigationToPost(postId, 0);
            logJumpState("thread-after-lock", threadGuard, postId);
            threadGuard->ensurePostVisible(postId);
            logJumpState("thread-after-ensure", threadGuard, postId);
            threadGuard->goToPost(postId);
            logJumpState("thread-after-go", threadGuard, postId);
            scheduleJumpState(threadGuard, postId, 0, "thread-t+0");
            scheduleJumpState(threadGuard, postId, 50, "thread-t+50");
            scheduleJumpState(threadGuard, postId, 250, "thread-t+250");
            scheduleJumpState(threadGuard, postId, 1000, "thread-t+1000");
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

            logJumpState("before-context", areaGuard, postId);
            bool contextReady = false;
            if (!contextPostIds.isEmpty()) {
                contextReady = areaGuard->ensurePinnedPostVisible(postId, contextPostIds,
                                                                  reachedOldest, reachedNewest);
            } else {
                contextReady = areaGuard->ensurePostVisible(postId);
            }
            qCWarning(lcJumpTrace).nospace()
                << "CONTEXT_RESULT postId=" << postId
                << " ready=" << contextReady
                << " contextCount=" << contextPostIds.size();
            logJumpState("after-context", areaGuard, postId);
            if (!contextReady) {
                return;
            }

            areaGuard->lockNavigationToPost(postId, 0);
            logJumpState("after-lock", areaGuard, postId);
            areaGuard->goToPost(postId);
            logJumpState("after-go", areaGuard, postId);

            scheduleJumpState(areaGuard, postId, 0, "t+0");
            scheduleJumpState(areaGuard, postId, 50, "t+50");
            scheduleJumpState(areaGuard, postId, 250, "t+250");
            scheduleJumpState(areaGuard, postId, 1000, "t+1000");
        });
}

} // namespace Mattermost
