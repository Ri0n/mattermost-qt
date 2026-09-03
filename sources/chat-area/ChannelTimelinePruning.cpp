#include "ChannelTimelineController.h"

#include <algorithm>

#include <QPointer>
#include <QScrollBar>

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
    if (!active || timeline.loadedCount() <= MaxMaterializedPosts) {
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

    const QVector<int> removed = timeline.pruneLoadedToNearest(
        centerIndex, MaxMaterializedPosts);
    if (removed.isEmpty()) {
        return;
    }

    // Page bookkeeping describes materialized pages, not the backend cache.
    // Once any rows are evicted, let a future gap seek reload whatever page it
    // needs instead of incorrectly treating an old page number as still present.
    loadedPages.clear();

    // The PostsListWidget reconciliation path removes only the now-remote rows,
    // expands the corresponding gaps, and keeps every surviving PostWidget at
    // the same identity. The captured visible-post/pixel anchor is restored in
    // the same synchronous transaction.
    renderTimeline(QString(), anchor);
}

} // namespace Mattermost
