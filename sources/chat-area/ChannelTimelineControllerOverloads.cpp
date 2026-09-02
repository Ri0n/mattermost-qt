#include "ChannelTimelineController.h"

namespace Mattermost {

void ChannelTimelineController::renderTimeline()
{
    renderTimeline(QString(), ViewportAnchor());
}

void ChannelTimelineController::renderTimeline(const QString& focusPostId)
{
    renderTimeline(focusPostId, ViewportAnchor());
}

} // namespace Mattermost
