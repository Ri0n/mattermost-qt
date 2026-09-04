#include "ChannelTimelineController.h"

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

void ChannelTimelineController::schedulePrune()
{
    // A seek generation owns its materialized window until it has finished
    // seed/edge loading and geometry refinement. Evicting rows during that
    // transaction is exactly the race that can make a just-loaded window appear
    // briefly and then disappear. finishSeek() schedules pruning afterwards.
    if (!active || timeline.loadedCount() <= MaxMaterializedPosts
        || seekState.isActive(seekState.currentTicket())) {
        return;
    }

    const quint64 requestGeneration = ++pruneGeneration;
    QPointer<ChannelTimelineController> guard(this);
    QTimer::singleShot(PruneIdleMs, this, [guard, requestGeneration] {
        if (guard) {
            guard->pruneLoadedPosts(requestGeneration);
        }
    });
}

int ChannelTimelineController::logicalIndexForAnchor(const ViewportAnchor& anchor) const
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
        return std::max(0,
            std::min(timeline.totalCount() - 1,
                     timeline.totalCount() - 1 - anchor.distanceFromNewest));
    case ViewportAnchor::None:
        break;
    }
    return timeline.totalCount() - 1;
}

void ChannelTimelineController::pruneLoadedPosts(quint64 pruneRequestGeneration)
{
    if (!active || pruneRequestGeneration != pruneGeneration
        || timeline.loadedCount() <= MaxMaterializedPosts
        || requestInFlight || rebuilding || !area.ui || !area.ui->listWidget) {
        return;
    }

    if (seekState.isActive(seekState.currentTicket())) {
        // Do not keep a second pruning timer racing the seek. finishSeek() will
        // schedule a fresh idle prune for the final viewport/window.
        return;
    }

    PostsListWidget* list = area.ui->listWidget;
    if (list->verticalScrollBar()->isSliderDown()
        || list->hasTimelineNavigationLock()) {
        schedulePrune();
        return;
    }

    const ViewportAnchor anchor = stableViewportAnchor();
    const int centerIndex = logicalIndexForAnchor(anchor);
    if (centerIndex < 0) {
        return;
    }

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

    // Page bookkeeping describes materialized pages, not the backend cache.
    // Once any rows are evicted, let a future gap seek reload whatever page it
    // needs instead of incorrectly treating an old page number as still present.
    loadedPages.clear();

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

    restoreViewportAnchor(anchor);
    paintWidget->setUpdatesEnabled(true);
    list->viewport()->update();
}

} // namespace Mattermost
