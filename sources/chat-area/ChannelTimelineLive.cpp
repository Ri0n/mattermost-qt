#include "ChannelTimelineController.h"

#include <climits>

#include <QDateTime>

#include "ChatArea.h"
#include "PostsListWidget.h"
#include "backend/types/BackendChannel.h"
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

    // REST/context loading may already have delivered the same identity before
    // the websocket event arrives. In that case neither logical count, unread
    // state nor UI rows may advance again.
    if (timeline.contains(post.id)) {
        return;
    }

    PostsListWidget* list = area.ui ? area.ui->listWidget : nullptr;

    // A websocket root post extends the logical history at the newest edge.
    // Existing indices stay fixed; the new post occupies exactly one new final
    // slot. This is deliberately different from oldest-edge growth.
    if (expectedPostCount < INT_MAX) {
        ++expectedPostCount;
    }
    timeline.setTotalCount(expectedPostCount);
    timeline.placeWindow(expectedPostCount - 1, QStringList {post.id});

    // Absolute Mattermost page=N windows are newest-relative. Adding one tail
    // post shifts every page boundary, so cached page numbers are no longer an
    // authoritative statement that a later logical gap is already covered.
    // Keep the materialized rows and sequential nextOlderPage cursor, but let
    // future random seeks reload whatever absolute page now owns their index.
    loadedPages.clear();

    // Gap anchors are newest-relative because oldest-edge growth shifts the
    // whole logical coordinate system. Newest-edge growth is the opposite:
    // increase the stored distance so the same gap location remains fixed.
    if (lastUserViewportAnchor.kind == ViewportAnchor::Gap
        && lastUserViewportAnchor.distanceFromNewest >= 0
        && lastUserViewportAnchor.distanceFromNewest < INT_MAX) {
        ++lastUserViewportAnchor.distanceFromNewest;
    }

    if (!initialRenderDone || !list) {
        scheduleMeasurementPass();
        schedulePrune();
        return;
    }

    const int previousLogicalIndex = timeline.indexOf(post.id) - 1;

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
