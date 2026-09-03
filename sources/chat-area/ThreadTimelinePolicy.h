#pragma once

#include <algorithm>

#include <QString>

namespace Mattermost {

inline int threadExpectedCountAfterLiveReply(int currentExpected,
                                             int reportedExpected,
                                             bool replyAlreadyMaterialized)
{
    Q_UNUSED(replyAlreadyMaterialized);

    const int current = std::max(1, currentExpected);
    const int reported = std::max(1, reportedExpected);

    // BackendChannel::addPost() increments rootPost->reply_count and emits
    // onPostEdited(root) before WebSocketEventHandler emits onNewPost(reply).
    // Therefore reportedExpected already includes this reply by the time the
    // thread controller sees the live-post signal. Growing once more here is
    // exactly what manufactures a one-row gap immediately before the newest
    // message. The live event only materializes the newest reported slot.
    return std::max(current, reported);
}

inline bool threadPageConfirmsNewestBoundary(bool hasNext,
                                             const QString& nextPostId,
                                             int responseSize,
                                             int pageSize)
{
    // Older Mattermost versions may omit has_next. A short page with no
    // next_post_id is the conservative cross-version proof that this is the real
    // newest edge. Full pages remain ambiguous and must keep their trailing gap.
    return !hasNext
        && nextPostId.isEmpty()
        && responseSize >= 0
        && responseSize < std::max(1, pageSize);
}

} // namespace Mattermost
