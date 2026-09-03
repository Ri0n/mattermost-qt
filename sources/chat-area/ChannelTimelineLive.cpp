#include "ChannelTimelineController.h"

#include <climits>

#include <QDateTime>

#include "ChatArea.h"
#include "PostsListWidget.h"
#include "backend/types/BackendPost.h"
#include "channel-tree/ChannelItem.h"
#include "post/PostWidget.h"
#include "ui_ChatArea.h"

namespace Mattermost {

void ChannelTimelineController::materializeLivePost(BackendPost& post)
{
    if (!active || post.hidden || !post.root_id.isEmpty()) {
        return;
    }

    const bool alreadyLogical = timeline.contains(post.id);
    PostsListWidget* list = area.ui ? area.ui->listWidget : nullptr;
    const bool alreadyVisible = list && list->findPost(post.id);
    if (alreadyLogical && alreadyVisible) {
        return;
    }

    if (!alreadyLogical) {
        // A websocket root post extends the logical history at the newest edge.
        // Existing indices stay fixed; the new post occupies exactly one new
        // final slot. Re-delivery of the same ID is therefore idempotent.
        if (expectedPostCount < INT_MAX) {
            ++expectedPostCount;
        }
        timeline.setTotalCount(expectedPostCount);
        timeline.placeWindow(expectedPostCount - 1, QStringList {post.id});

        // Gap anchors are newest-relative because oldest-edge growth shifts the
        // whole logical coordinate system. Newest-edge growth is the opposite:
        // increase the stored distance so the same gap location remains fixed.
        if (lastUserViewportAnchor.kind == ViewportAnchor::Gap
            && lastUserViewportAnchor.distanceFromNewest >= 0
            && lastUserViewportAnchor.distanceFromNewest < INT_MAX) {
            ++lastUserViewportAnchor.distanceFromNewest;
        }
    }

    if (!initialRenderDone || !list || alreadyVisible) {
        scheduleMeasurementPass();
        schedulePrune();
        return;
    }

    int previousLogicalIndex = timeline.indexOf(post.id) - 1;

    // Only add a date separator when the immediate logical predecessor is also
    // materialized. If a sparse gap precedes the live row, that gap may contain
    // any number of date boundaries and will recreate them when loaded later.
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

    // Preserve the legacy unread semantics, but keep them in the same atomic
    // live-post transaction as the row insertion so no second slot can mutate
    // QListWidget geometry for the same websocket event.
    const bool chatAreaHasFocus = area.treeItem
        && area.treeItem->isSelected()
        && area.isActiveWindow();
    if (!chatAreaHasFocus) {
        list->addNewMessagesSeparator();
    }

    auto* postWidget = new PostWidget(area.backend, post, list, &area, nullptr);
    list->insertPost(postWidget);
    connect(postWidget, &PostWidget::dimensionsChanged,
            this, &ChannelTimelineController::scheduleMeasurementPass);

    area.lastPostDate = post.getCreationTime().date();
    area.areaIsFilled = timeline.loadedCount() > 20;

    if (!area.pendingPostId.isEmpty()) {
        area.goToPost(area.pendingPostId);
    }

    if (!chatAreaHasFocus) {
        ++area.unreadMessagesCount;
        area.setUnreadMessagesCount(area.unreadMessagesCount);
    }

    // PostsListWidget::insertPost() already preserves its concrete saved anchor
    // and follows the tail only while the explicit sticky-bottom marker is set.
    // Never call adjustSize(), renderTimeline() or scrollToBottom() here: one
    // websocket post is a one-row mutation, not a full timeline transaction.
    scheduleMeasurementPass();
    schedulePrune();
}

} // namespace Mattermost
