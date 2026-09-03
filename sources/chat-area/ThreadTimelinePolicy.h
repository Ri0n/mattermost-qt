#pragma once

#include <algorithm>
#include <climits>

#include <QString>

namespace Mattermost {

inline int threadExpectedCountAfterLiveReply(int currentExpected,
                                             int reportedExpected,
                                             bool replyAlreadyMaterialized)
{
    const int current = std::max(1, currentExpected);
    const int reported = std::max(1, reportedExpected);

    if (replyAlreadyMaterialized) {
        return std::max(current, reported);
    }

    // Mattermost may update the root post's reply_count before delivering the
    // websocket post event. In that ordering the reported count already includes
    // the live reply, so incrementing current once more manufactures a one-row
    // gap immediately before the newest message.
    if (reported > current) {
        return reported;
    }

    return current < INT_MAX ? current + 1 : current;
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
