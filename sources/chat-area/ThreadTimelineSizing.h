#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace Mattermost {

/**
 * Convert Mattermost's live-reply metadata into the number of visible logical
 * thread rows. Deleted replies that the client still renders as tombstones are
 * deliberately additional rows: reply_count tracks live replies, not those
 * locally retained placeholders.
 */
inline int threadLogicalItemCount(std::int64_t liveReplies,
                                  int deletedReplyTombstones)
{
    const std::int64_t boundedLive = std::max<std::int64_t>(0, liveReplies);
    const std::int64_t boundedDeleted = std::max<std::int64_t>(0, deletedReplyTombstones);
    const std::int64_t maxReplies = std::numeric_limits<int>::max() - 1LL;
    const std::int64_t visibleReplies = std::min(
        maxReplies,
        boundedLive > maxReplies - std::min(boundedDeleted, maxReplies)
            ? maxReplies
            : boundedLive + boundedDeleted);
    return std::max(1, static_cast<int>(visibleReplies) + 1);
}

} // namespace Mattermost
