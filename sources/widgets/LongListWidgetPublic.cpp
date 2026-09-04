#include "LongListWidget.h"

namespace Mattermost {

void LongListWidget::finishRangeRequest(int first, int last)
{
    clearPendingRequest(first, last);
    scheduleSync(seekActive ? RequestReason::Seek : RequestReason::Scroll);
}

bool LongListWidget::isAtEnd() const
{
    return maximumContentOffset() - contentOffset() <= 2;
}

} // namespace Mattermost
