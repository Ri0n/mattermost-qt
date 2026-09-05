#include "ChatArea.h"

#include "ChannelPostSource.h"
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
    // actual viewport lock: Center is applied once, then the target post's top
    // keeps the same screen Y through reflow and the same Y/viewportHeight ratio
    // through window resize. Authoritative source remaps only change the locked
    // logical index; they never recenter the post.
    ui->listWidget->lockNavigationToPost(postId,
                                         LongListWidget::Alignment::Center,
                                         quietPeriodMs);
}

} // namespace Mattermost
