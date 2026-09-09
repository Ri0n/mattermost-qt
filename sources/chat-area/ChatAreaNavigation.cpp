#include "ChatArea.h"

#include <memory>
#include <utility>

#include <QPointer>

#include "ChannelPostSource.h"
#include "ChatLogWidget.h"
#include "ThreadPostSource.h"
#include "ui_ChatArea.h"

namespace Mattermost {

bool ChatArea::ensurePostVisible(const QString& postId)
{
    if (postId.isEmpty() || !ui || !ui->listWidget) {
        return false;
    }
    return ui->listWidget->ensurePostVisible(postId);
}

bool ChatArea::ensurePinnedPostVisible(const QString& postId,
                                       const QStringList& contextPostIds,
                                       bool reachedOldest,
                                       bool reachedNewest)
{
    if (postId.isEmpty() || !ui || !ui->listWidget) {
        return false;
    }

    auto* source = qobject_cast<ChannelPostSource*>(ui->listWidget->source());
    if (!source) {
        return ensurePostVisible(postId);
    }

    // ChannelPostSource owns logical identity placement. Publish the complete
    // request-local context atomically before LongListWidget is asked to center
    // or lock the target, so no single guessed row can flash on screen first.
    return source->adoptNavigationContext(postId, contextPostIds,
                                          reachedOldest, reachedNewest);
}

void ChatArea::lockNavigationToPost(const QString& postId, int quietPeriodMs)
{
    if (postId.isEmpty() || !ui || !ui->listWidget) {
        return;
    }

    // ChatLogWidget owns only semantic post identity. LongListWidget owns the
    // actual viewport lock: fitting targets stay centred as their own geometry
    // settles, oversized targets are top-aligned, and unrelated reflow preserves
    // the target's current screen Y. Authoritative source remaps only change the
    // locked logical identity without creating a visible jump.
    ui->listWidget->lockNavigationToPost(postId,
                                         LongListWidget::Alignment::Center,
                                         quietPeriodMs);
}

void ChatArea::highlightPostWhenAuthoritative(const QString& postId,
                                              std::function<void()> onPresented)
{
    if (postId.isEmpty() || !ui || !ui->listWidget) {
        return;
    }

    auto* source = qobject_cast<ThreadPostSource*>(ui->listWidget->source());
    if (!source || source->isPostPositionAuthoritative(postId)) {
        ui->listWidget->highlightPost(postId);
        if (onPresented) {
            onPresented();
        }
        return;
    }

    // A cold permalink can know the reply body before its exact logical thread
    // position. Flashing that provisional widget races the authoritative page:
    // the widget is immediately rematerialized and the animation disappears.
    // Every range request completes after any exact-window placement, so wait
    // for that semantic confirmation rather than using an arbitrary timer.
    const std::uint64_t generation = viewportNavigationGeneration;
    QPointer<ChatArea> guard(this);
    auto connection = std::make_shared<QMetaObject::Connection>();
    *connection = connect(source, &AbstractPostSource::rangeRequestFinished,
                          this,
                          [guard, source, postId, generation, connection,
                           onPresented = std::move(onPresented)](int, int) mutable {
        if (!guard || generation != guard->viewportNavigationGeneration) {
            QObject::disconnect(*connection);
            return;
        }
        if (!source->isPostPositionAuthoritative(postId)) {
            return;
        }

        QObject::disconnect(*connection);
        if (guard->ui && guard->ui->listWidget) {
            guard->ui->listWidget->highlightPost(postId);
            if (onPresented) {
                onPresented();
            }
        }
    });
}

} // namespace Mattermost
