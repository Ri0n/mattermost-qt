#include "ThreadTimelineController.h"

#include <algorithm>

#include <QMap>
#include <QPointer>
#include <QScrollBar>
#include <QSignalBlocker>

#include "ChatArea.h"
#include "PostsListWidget.h"
#include "ui_ChatArea.h"

namespace Mattermost {
namespace {

constexpr int MaxMaterializedPosts = 200;
constexpr int PruneIdleMs = 350;

} // namespace

void ThreadTimelineController::schedulePrune()
{
    if (timeline.loadedCount() <= MaxMaterializedPosts
        || seekState.isActive(seekState.currentTicket())) {
        return;
    }

    const quint64 requestGeneration = ++pruneGeneration;
    QPointer<ThreadTimelineController> guard(this);
    QTimer::singleShot(PruneIdleMs, this, [guard, requestGeneration] {
        if (guard) {
            guard->pruneLoadedPosts(requestGeneration);
        }
    });
}

int ThreadTimelineController::logicalIndexForAnchor(const ViewportAnchor& anchor) const
{
    if (timeline.totalCount() <= 0) {
        return -1;
    }

    switch (anchor.kind) {
    case ViewportAnchor::Bottom:
        return timeline.totalCount() - 1;
    case ViewportAnchor::Post: {
        const int index = timeline.indexOf(anchor.postId);
        return index >= 0 ? index : timeline.totalCount() - 1;
    }
    case ViewportAnchor::Gap:
        return std::max(0, std::min(timeline.totalCount() - 1, anchor.logicalIndex));
    case ViewportAnchor::None:
        break;
    }
    return timeline.totalCount() - 1;
}

void ThreadTimelineController::pruneLoadedPosts(quint64 pruneRequestGeneration)
{
    if (pruneRequestGeneration != pruneGeneration
        || timeline.loadedCount() <= MaxMaterializedPosts
        || requestInFlight || rebuilding || !area.ui || !area.ui->listWidget) {
        return;
    }

    if (seekState.isActive(seekState.currentTicket())) {
        // finishSeek() owns the next prune scheduling. Never evict the window
        // while a seed/edge/measurement transaction still owns the viewport.
        return;
    }

    PostsListWidget* list = area.ui->listWidget;
    if (list->verticalScrollBar()->isSliderDown()
        || list->hasTimelineNavigationLock()) {
        schedulePrune();
        return;
    }

    const ViewportAnchor anchor = captureViewportAnchor();
    const int centerIndex = logicalIndexForAnchor(anchor);
    if (centerIndex < 0) {
        return;
    }

    // Save identities before PostTimeline drops them. The UI mutation below
    // replaces only those exact rows with gap geometry; retained PostWidgets are
    // never described again or touched.
    QMap<int, QString> postIdByLogicalIndex;
    for (const PostTimeline::Span& span : timeline.spans()) {
        if (span.kind != PostTimeline::LoadedSpan) {
            continue;
        }
        for (int offset = 0; offset < span.postIds.size(); ++offset) {
            postIdByLogicalIndex.insert(span.firstIndex + offset,
                                        span.postIds.at(offset));
        }
    }

    const QVector<int> removed = timeline.pruneLoadedToNearest(
        centerIndex, MaxMaterializedPosts);
    if (removed.isEmpty()) {
        return;
    }

    const int estimatedRowHeight = timeline.estimatedRowHeight();
    QWidget* paintWidget = list;
    paintWidget->setUpdatesEnabled(false);
    const QSignalBlocker scrollSignals(list->verticalScrollBar());

    for (int logicalIndex : removed) {
        const QString postId = postIdByLogicalIndex.value(logicalIndex);
        if (!postId.isEmpty()) {
            list->evictTimelinePostToGap(postId, logicalIndex, estimatedRowHeight);
        }
    }

    // Restore the same concrete post/gap/bottom anchor while painting and
    // scrollbar signals are still frozen. This avoids both a visible jump and
    // an automatic gap seek caused solely by pruning geometry changes.
    restoreViewportAnchor(anchor);
    paintWidget->setUpdatesEnabled(true);
    list->viewport()->update();
    persistState();
}

} // namespace Mattermost
