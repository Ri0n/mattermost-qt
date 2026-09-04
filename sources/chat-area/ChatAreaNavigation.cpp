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

    // The Mattermost-specific view owns semantic post identity. LongListWidget
    // still owns every pixel/scrollbar operation; if an authoritative page moves
    // this post from an estimated slot, ChatLogWidget recenters the new logical
    // index until a real user gesture (or the quiet-period timeout) releases it.
    ui->listWidget->lockNavigationToPost(postId,
                                         LongListWidget::Alignment::Center,
                                         quietPeriodMs);
}

} // namespace Mattermost
