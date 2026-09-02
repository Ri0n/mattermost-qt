#include "ChannelTimelineController.h"

#include <QDateTime>

#include "ChatArea.h"
#include "backend/types/BackendChannel.h"

namespace Mattermost {

void ChannelTimelineController::renderTimeline()
{
    renderTimeline(QString(), ViewportAnchor());

    // The sparse service deliberately bypasses BackendChannel::onNewPosts and
    // ChatArea::fillChannelPosts(). Restore the two lifecycle effects that used
    // to happen there after the initial channel page became visible.
    if (area.channel.last_post_at != 0) {
        area.lastPostDate = QDateTime::fromMSecsSinceEpoch(area.channel.last_post_at).date();
    }
    area.requestExplicitReadAcknowledgement();
}

void ChannelTimelineController::renderTimeline(const QString& focusPostId)
{
    renderTimeline(focusPostId, ViewportAnchor());
}

} // namespace Mattermost
