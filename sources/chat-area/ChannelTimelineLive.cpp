#include "ChannelTimelineController.h"

#include <QDateTime>

#include "ChatArea.h"
#include "PostsListWidget.h"
#include "backend/types/BackendPost.h"
#include "post/PostWidget.h"
#include "ui_ChatArea.h"

namespace Mattermost {

void ChannelTimelineController::materializeLivePost(BackendPost& post)
{
    if (!active || post.hidden || !post.root_id.isEmpty()
        || !initialRenderDone || !area.ui || !area.ui->listWidget) {
        return;
    }

    PostsListWidget* list = area.ui->listWidget;
    if (list->findPost(post.id)) {
        return;
    }

    // This method deliberately does not modify PostTimeline. The controller's
    // existing onNewPost handler owns logical newest-edge insertion; this method
    // owns only the physical one-row QListWidget insertion. Keeping those two
    // responsibilities idempotent makes signal connection order irrelevant.
    int previousLogicalIndex = timeline.totalCount() - 1;
    const int liveLogicalIndex = timeline.indexOf(post.id);
    if (liveLogicalIndex >= 0) {
        previousLogicalIndex = liveLogicalIndex - 1;
    }

    // Only decorate with a day separator when the live row is directly adjacent
    // to an already materialized predecessor. If a sparse gap precedes it, that
    // gap may contain arbitrary date boundaries and reconciliation will create
    // the correct separators when those rows are materialized later.
    if (previousLogicalIndex >= 0) {
        const QString previousId = timeline.postIdAt(previousLogicalIndex);
        BackendPost* previous = previousId.isEmpty()
            ? nullptr : area.channel.postIdToPost.value(previousId, nullptr);
        if (previous
            && previous->getCreationTime().date() != post.getCreationTime().date()) {
            const int daysAgo = post.getCreationTime().date()
                .daysTo(QDateTime::currentDateTime().date());
            list->addDaySeparator(daysAgo);
        }
    }

    auto* postWidget = new PostWidget(area.backend, post, list, &area, nullptr);
    list->insertPost(postWidget);
    connect(postWidget, &PostWidget::dimensionsChanged,
            this, &ChannelTimelineController::scheduleMeasurementPass);

    area.lastPostDate = post.getCreationTime().date();

    // insertPost() preserves the concrete saved viewport anchor and follows the
    // newest edge only when PostsListWidget's explicit sticky-bottom marker says
    // so. Never call adjustSize(), renderTimeline() or scrollToBottom() here:
    // a websocket message is a one-row mutation, not a timeline transaction.
    scheduleMeasurementPass();
    schedulePrune();
}

} // namespace Mattermost
