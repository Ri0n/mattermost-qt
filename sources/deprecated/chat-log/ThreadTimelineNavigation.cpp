#include "ThreadTimelineController.h"

#include <algorithm>
#include <cmath>

#include "ChatArea.h"
#include "PostsListWidget.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "ui_ChatArea.h"

namespace Mattermost {
namespace {

bool hasPostRow(const PostsListWidget* list, const QString& postId)
{
    if (!list || postId.isEmpty()) {
        return false;
    }

    for (int row = 0; row < list->count(); ++row) {
        const QListWidgetItem* item = list->item(row);
        if (PostsListWidget::isPostItem(item)
            && item->data(ItemRole::postId).toString() == postId) {
            return true;
        }
    }
    return false;
}

int nearestUnloadedIndex(const PostTimeline& timeline, int desiredIndex)
{
    const int count = timeline.totalCount();
    if (count <= 1) {
        return -1;
    }

    desiredIndex = std::max(1, std::min(count - 1, desiredIndex));
    if (timeline.postIdAt(desiredIndex).isEmpty()) {
        return desiredIndex;
    }

    for (int distance = 1; distance < count; ++distance) {
        const int before = desiredIndex - distance;
        if (before >= 1 && timeline.postIdAt(before).isEmpty()) {
            return before;
        }

        const int after = desiredIndex + distance;
        if (after < count && timeline.postIdAt(after).isEmpty()) {
            return after;
        }
    }
    return -1;
}

} // namespace

bool ThreadTimelineController::ensurePostVisible(const QString& postId)
{
    if (postId.isEmpty() || !area.ui || !area.ui->listWidget) {
        return false;
    }

    PostsListWidget* list = area.ui->listWidget;
    if (hasPostRow(list, postId)) {
        return true;
    }

    BackendPost* target = area.channel.postIdToPost.value(postId, nullptr);
    if (!target || target->hidden || target->root_id != rootId) {
        return false;
    }

    // Permalink resolution has already fetched the exact reply through
    // PostNavigationService. The sparse thread model may nevertheless know only
    // its initial page. Place the cached reply into the closest logical gap so
    // the user can navigate immediately; ordinary gap prefetch then fills the
    // surrounding thread context.
    int totalCount = std::max(expectedPostCount, timeline.totalCount());
    if (totalCount < 2) {
        totalCount = 2;
    }
    if (timeline.totalCount() != totalCount) {
        expectedPostCount = totalCount;
        timeline.setTotalCount(totalCount);
    }

    if (!timeline.contains(postId)) {
        int desiredIndex = 1;
        BackendPost* root = area.channel.postIdToPost.value(rootId, nullptr);
        if (root && root->last_reply_at > root->create_at && totalCount > 2) {
            const long double span = static_cast<long double>(
                root->last_reply_at - root->create_at);
            const long double offset = target->create_at <= root->create_at
                ? 0.0L
                : static_cast<long double>(target->create_at - root->create_at);
            const long double fraction = std::clamp(offset / span, 0.0L, 1.0L);
            desiredIndex = 1 + static_cast<int>(std::llround(
                fraction * static_cast<long double>(totalCount - 2)));
        } else if (nextLogicalIndex > 1) {
            desiredIndex = std::min(totalCount - 1, nextLogicalIndex);
        }

        int logicalIndex = nearestUnloadedIndex(timeline, desiredIndex);
        if (logicalIndex < 0) {
            // A completely materialized model that does not contain this cached
            // reply is stale by one logical row. Grow only at the newest edge;
            // later server pagination will collapse speculative geometry again.
            logicalIndex = timeline.totalCount();
            expectedPostCount = logicalIndex + 1;
            timeline.setTotalCount(expectedPostCount);
        }
        timeline.placeWindow(logicalIndex, QStringList {postId});
    }

    renderTimeline(postId, true);
    scheduleViewportCheck();
    return hasPostRow(list, postId);
}

} // namespace Mattermost
