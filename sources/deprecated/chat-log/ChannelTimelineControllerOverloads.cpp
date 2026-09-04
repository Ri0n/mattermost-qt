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

    // This overload is the initial ordinary-channel render. Page 0 is the newest
    // server page, so make the newest materialized span authoritative even if a
    // stale/approximate total count temporarily left geometry after it. A normal
    // channel open must never put the scrollbar at the end of a gap with the
    // actual newest PostWidgets sitting above that gap.
    QString newestMaterializedPostId;
    const QVector<PostTimeline::Span> spans = timeline.spans();
    for (auto it = spans.crbegin(); it != spans.crend(); ++it) {
        if (it->kind == PostTimeline::LoadedSpan && !it->postIds.isEmpty()) {
            newestMaterializedPostId = it->postIds.last();
            break;
        }
    }
    if (!newestMaterializedPostId.isEmpty()) {
        timeline.alignLoadedSpanToBoundary(newestMaterializedPostId, false);
    }

    ViewportAnchor bottomAnchor;
    bottomAnchor.kind = ViewportAnchor::Bottom;
    renderTimeline(QString(), bottomAnchor);

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
