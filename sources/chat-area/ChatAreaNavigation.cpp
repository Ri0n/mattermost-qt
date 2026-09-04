#include "ChatArea.h"

#include "AbstractPostSource.h"
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
    Q_UNUSED(quietPeriodMs)

    if (postId.isEmpty() || !ui || !ui->listWidget || !ui->listWidget->source()) {
        return;
    }

    // LongListWidget owns a durable item anchor across later geometry changes.
    // Ensure the source has a logical slot before the following goToPost() call.
    ui->listWidget->source()->ensurePostIndex(postId);
}

} // namespace Mattermost
