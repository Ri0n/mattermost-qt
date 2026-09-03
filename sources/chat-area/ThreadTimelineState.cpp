#include "ThreadTimelineController.h"

#include <algorithm>

#include <QHash>

#include "ChatArea.h"
#include "backend/Backend.h"
#include "backend/types/BackendChannel.h"

namespace Mattermost {
namespace {

struct SavedThreadTimelineState {
    PostTimeline timeline;
    QString cursorPostId;
    uint64_t cursorCreateAt = 0;
    int expectedPostCount = 1;
    int nextLogicalIndex = 0;
    int anchorKind = 0;
    QString anchorPostId;
    int anchorPostTopOffset = 0;
    int anchorLogicalIndex = -1;
    int anchorOffsetWithinEstimatedRow = 0;
    bool hasNext = true;
    bool initialPrefetchDone = false;
    bool valid = false;
};

using PerBackendThreadState = QHash<QString, SavedThreadTimelineState>;

QHash<Backend*, PerBackendThreadState>& savedThreadStates()
{
    static QHash<Backend*, PerBackendThreadState> states;
    return states;
}

} // namespace

void ThreadTimelineController::persistState()
{
    if (rootId.isEmpty() || !area.ui || !area.ui->listWidget) {
        return;
    }

    SavedThreadTimelineState state;
    state.timeline = timeline;
    state.cursorPostId = cursorPostId;
    state.cursorCreateAt = cursorCreateAt;
    state.expectedPostCount = expectedPostCount;
    state.nextLogicalIndex = nextLogicalIndex;
    state.hasNext = hasNext;
    state.initialPrefetchDone = initialPrefetchDone;

    const ViewportAnchor anchor = captureViewportAnchor();
    state.anchorKind = static_cast<int>(anchor.kind);
    state.anchorPostId = anchor.postId;
    state.anchorPostTopOffset = anchor.postTopOffset;
    state.anchorOffsetWithinEstimatedRow = anchor.offsetWithinEstimatedRow;
    if (anchor.kind == ViewportAnchor::Post) {
        state.anchorLogicalIndex = timeline.indexOf(anchor.postId);
    } else if (anchor.kind == ViewportAnchor::Gap) {
        state.anchorLogicalIndex = anchor.logicalIndex;
    } else if (anchor.kind == ViewportAnchor::Bottom && timeline.totalCount() > 0) {
        state.anchorLogicalIndex = timeline.totalCount() - 1;
    }
    state.valid = anchor.isValid() || timeline.loadedCount() > 0;

    // This cache contains only the sparse logical model (at most the 200
    // materialized IDs plus gaps), never PostWidget/QPixmap/UI objects. Closed
    // thread windows can therefore restore their reading position without
    // retaining the expensive QWidget tree.
    savedThreadStates()[&area.backend].insert(rootId, state);
}

bool ThreadTimelineController::restoreSavedState(ViewportAnchor& anchor)
{
    auto backendIt = savedThreadStates().find(&area.backend);
    if (backendIt == savedThreadStates().end()) {
        return false;
    }

    auto stateIt = backendIt->find(rootId);
    if (stateIt == backendIt->end() || !stateIt->valid) {
        return false;
    }

    const SavedThreadTimelineState& state = stateIt.value();
    timeline = state.timeline;
    cursorPostId = state.cursorPostId;
    cursorCreateAt = state.cursorCreateAt;
    expectedPostCount = std::max(1, state.expectedPostCount);
    nextLogicalIndex = std::max(0, state.nextLogicalIndex);
    hasNext = state.hasNext;
    initialPrefetchDone = state.initialPrefetchDone;

    const int kind = state.anchorKind;
    if (kind >= static_cast<int>(ViewportAnchor::None)
        && kind <= static_cast<int>(ViewportAnchor::Gap)) {
        anchor.kind = static_cast<ViewportAnchor::Kind>(kind);
    }
    anchor.postId = state.anchorPostId;
    anchor.postTopOffset = state.anchorPostTopOffset;
    anchor.logicalIndex = state.anchorLogicalIndex;
    anchor.offsetWithinEstimatedRow = state.anchorOffsetWithinEstimatedRow;

    if (anchor.kind == ViewportAnchor::Post
        && (anchor.postId.isEmpty()
            || !area.channel.postIdToPost.contains(anchor.postId))) {
        // The backend cache normally outlives the thread window, but if the post
        // was evicted for another reason retain the logical reading position and
        // let the sparse seek path materialize it again.
        anchor.kind = ViewportAnchor::Gap;
        anchor.postId.clear();
    }

    return true;
}

} // namespace Mattermost
