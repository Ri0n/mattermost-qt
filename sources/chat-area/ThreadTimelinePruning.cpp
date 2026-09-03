#include "ThreadTimelineController.h"

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

void ThreadTimelineController::schedulePrune()
{
    if (timeline.loadedCount() <= MaxMaterializedPosts) {
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

    const QVector<int> removed = timeline.pruneLoadedToNearest(
        centerIndex, MaxMaterializedPosts);
    if (removed.isEmpty()) {
        return;
    }

    renderTimeline(QString(), false, anchor);
    persistState();
}

} // namespace Mattermost
