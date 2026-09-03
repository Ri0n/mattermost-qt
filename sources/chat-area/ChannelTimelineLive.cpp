#include "ChannelTimelineController.h"

#include <climits>

#include <QDateTime>

#include "ChatArea.h"
#include "PostsListWidget.h"
#include "backend/types/BackendPost.h"
#include "post/PostWidget.h"
#include "ui_ChatArea.h"

namespace Mattermost {

void ChannelTimelineController::appendLivePost(BackendPost& post)
{
    if (!active || post.hidden || !post.root_id.isEmpty()) {
        return;
    }

    const bool alreadyMaterialized = timeline.contains(post.id);
    if (!alreadyMaterialized) {
        // A websocket root post extends the logical history at the newest edge.
        // Keep every existing logical index unchanged and occupy exactly the new
        // last slot; unlike oldest-edge discovery this must never shift loaded
        // spans or rebuild the list.
        if (expectedPostCount < INT_MAX) {
            ++expectedPostCount;
        }
        timeline.setTotalCount(expectedPostCount);
        timeline.placeWindow(expectedPostCount - 1, QStringList {post.id});

        // Gap anchors are stored newest-relative so oldest-edge growth can move
        // beneath them safely. Newest-edge growth is the opposite operation: to
        // keep the same logical reading position, its distance from newest must
        // grow together with the new tail row.
        if (lastUserViewportAnchor.kind == ViewportAnchor::Gap
            && lastUserViewportAnchor.distanceFromNewest >= 0
            && lastUserViewportAnchor.distanceFromNewest < INT_MAX) {
            ++lastUserViewportAnchor.distanceFromNewest;
        }
    }

    if (!initialRenderDone || alreadyMaterialized || !area.ui || !area.ui->listWidget) {
        scheduleMeasurementPass();
        schedulePrune();
        return;
    }

    PostsListWidget* list = area.ui->listWidget;
    if (list->findPost(post.id)) {
        scheduleMeasurementPass();
        schedulePrune();
        return;
    }

    // Only decorate with a day separator when the live row is directly adjacent
    // to an already materialized predecessor. If a sparse gap precedes it, that
    // gap may contain arbitrary date boundaries and reconciliation will create
    // the correct separators when those rows are materialized later.
    const int logicalIndex = timeline.indexOf(post.id);
    if (logicalIndex > 0) {
        const QString previousId = timeline.postIdAt(logicalIndex - 1);
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
    area.areaIsFilled = timeline.loadedCount() > 20;

    // insertPost() already preserves the concrete saved viewport anchor and, if
    // the user is explicitly sticky at bottom, follows the new tail. Do not call
    // adjustSize(), renderTimeline() or scrollToBottom() here: all three would
    // turn a one-row websocket update into a whole-list geometry transaction.
    scheduleMeasurementPass();
    schedulePrune();
}

} // namespace Mattermost
