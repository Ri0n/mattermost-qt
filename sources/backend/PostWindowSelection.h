#pragma once

#include <algorithm>

#include <QString>
#include <QStringList>

namespace Mattermost {

/**
 * Select at most maxCount consecutive chronological post ids around targetId.
 *
 * The target is centered when both sides have enough data. If one side reaches
 * a timeline edge, its unused quota spills over to the other side so callers
 * still get a full window whenever enough posts exist in total.
 */
inline QStringList selectPostWindow(const QStringList& chronologicalIds,
                                    const QString& targetId,
                                    int maxCount)
{
    if (chronologicalIds.isEmpty() || targetId.isEmpty() || maxCount <= 0) {
        return {};
    }

    const int targetIndex = chronologicalIds.indexOf(targetId);
    if (targetIndex < 0) {
        return {};
    }

    const int count = std::min(maxCount, static_cast<int>(chronologicalIds.size()));
    const int preferredBefore = (count - 1) / 2;

    int first = std::max(0, targetIndex - preferredBefore);
    int lastExclusive = std::min(static_cast<int>(chronologicalIds.size()), first + count);

    // If the newest edge truncated the window, spend the unused quota before
    // the target instead of leaving the viewport unnecessarily sparse.
    first = std::max(0, lastExclusive - count);
    lastExclusive = std::min(static_cast<int>(chronologicalIds.size()), first + count);

    return chronologicalIds.mid(first, lastExclusive - first);
}

} // namespace Mattermost
