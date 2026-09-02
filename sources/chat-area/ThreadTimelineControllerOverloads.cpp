#include "ThreadTimelineController.h"

namespace Mattermost {

void ThreadTimelineController::renderTimeline(const QString& focusPostId, bool focusAtTop)
{
    renderTimeline(focusPostId, focusAtTop, ViewportAnchor());
}

} // namespace Mattermost
