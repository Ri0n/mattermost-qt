#include "ChatArea.h"

#include "ChatLogWidget.h"
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
    Q_UNUSED(contextPostIds)
    Q_UNUSED(reachedOldest)
    Q_UNUSED(reachedNewest)

    // PostNavigationService has already ingested the context into BackendChannel.
    // The source adopts the semantic target immediately and ordinary logical
    // range requests fill its neighbours without any UI-side gap bookkeeping.
    return ensurePostVisible(postId);
}

void ChatArea::lockNavigationToPost(const QString& postId, int quietPeriodMs)
{
    if (postId.isEmpty() || !ui || !ui->listWidget) {
        return;
    }

    // ChatLogWidget owns only semantic post identity. LongListWidget owns the
    // actual viewport lock: Center is applied once, then the target post's top
    // keeps the same screen Y through reflow and the same Y/viewportHeight ratio
    // through window resize. Authoritative source remaps only change the locked
    // logical index; they never recenter the post.
    ui->listWidget->lockNavigationToPost(postId,
                                         LongListWidget::Alignment::Center,
                                         quietPeriodMs);
}

} // namespace Mattermost
