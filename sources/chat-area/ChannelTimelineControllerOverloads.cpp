#include "ChannelTimelineController.h"

#include <QDateTime>

#include "ChatArea.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"

namespace Mattermost {

void ChannelTimelineController::renderTimeline()
{
    // From the first sparse render onward there must be exactly one owner of a
    // websocket root-post update. The legacy ChatArea slot inserts a row and then
    // calls adjustSize()/scrollToBottom(), while the original controller lambda
    // separately mutates PostTimeline. Replace both with one incremental handler.
    // Reinstall this ownership on every activation because deactivate() removes
    // all channel->controller connections before the next sparse startup.
    QObject::disconnect(&area.channel, &BackendChannel::onNewPost,
                        &area, &ChatArea::appendChannelPost);
    QObject::disconnect(&area.channel, &BackendChannel::onNewPost,
                        this, nullptr);
    connect(&area.channel, &BackendChannel::onNewPost,
            this, &ChannelTimelineController::materializeLivePost,
            Qt::UniqueConnection);

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
