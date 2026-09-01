/**
 * @file ChannelActivityTracker.cpp
 * @brief Tracks recent and unread channel activity independently of the UI.
 */

#include "ChannelActivityTracker.h"

#include <algorithm>

namespace Mattermost {

void ChannelActivityTracker::clear()
{
    entries.clear();
}

void ChannelActivityTracker::setMembership(const QString& channelId, uint64_t lastViewedAt,
                                           uint64_t readMessageCount, uint64_t readRootMessageCount,
                                           bool hasReadRootMessageCount, bool mentioned, bool muted)
{
    if (channelId.isEmpty()) {
        return;
    }

    Entry& entry = entries[channelId];
    entry.lastViewedAt = std::max(entry.lastViewedAt, lastViewedAt);
    entry.readMessageCount = std::max(entry.readMessageCount, readMessageCount);
    if (hasReadRootMessageCount) {
        entry.readRootMessageCount = std::max(entry.readRootMessageCount, readRootMessageCount);
        entry.hasReadRootMessageCount = true;
    }
    entry.mentioned = entry.mentioned || mentioned;
    entry.muted = muted;
    entry.tracked = true;
}

void ChannelActivityTracker::synchronizeChannel(const QString& channelId, uint64_t lastPostAt,
                                                uint64_t totalMessageCount, uint64_t totalRootMessageCount,
                                                bool hasTotalRootMessageCount)
{
    auto it = entries.find(channelId);
    if (it == entries.end() || !it->tracked) {
        return;
    }

    Entry& entry = it.value();
    entry.lastActivityAt = std::max(entry.lastActivityAt, lastPostAt);

    // Mattermost compares counters from the same domain. Root counters are only
    // usable when both the membership and the channel response provide them;
    // otherwise fall back to the ordinary total/msg_count pair. Mixing a root
    // membership counter with a non-root channel total creates false unreads on
    // servers that only expose one side of the CRT fields.
    const bool useRootCounts = entry.hasReadRootMessageCount && hasTotalRootMessageCount;
    const uint64_t readCount = useRootCounts ? entry.readRootMessageCount : entry.readMessageCount;
    const uint64_t totalCount = useRootCounts ? totalRootMessageCount : totalMessageCount;

    // The timestamp guard suppresses stale/inconsistent counters after a view.
    // A genuinely unread root post must also be newer than last_viewed_at.
    const bool unreadByCount = totalCount > readCount;
    const bool unreadByTime = lastPostAt > entry.lastViewedAt;
    entry.hasUnreadActivity = entry.hasUnreadActivity || (unreadByCount && unreadByTime);
}

void ChannelActivityTracker::recordPost(const QString& channelId, uint64_t createdAt, bool ownPost,
                                        bool threadReply, bool mentioned)
{
    if (channelId.isEmpty()) {
        return;
    }

    Entry& entry = entries[channelId];
    entry.tracked = true;
    entry.lastActivityAt = std::max(entry.lastActivityAt, createdAt);

    if (mentioned) {
        entry.mentioned = true;
    }

    if (ownPost) {
        return;
    }

    // Thread replies are not rendered in the parent channel. Match the existing
    // notification policy and only make the parent conversation require
    // attention when such a reply explicitly mentions the current user.
    if (!threadReply || mentioned) {
        entry.hasUnreadActivity = true;
    }
}

void ChannelActivityTracker::recordViewed(const QString& channelId, uint64_t viewedAt,
                                          uint64_t totalMessageCount, uint64_t totalRootMessageCount,
                                          bool hasTotalRootMessageCount)
{
    if (channelId.isEmpty()) {
        return;
    }

    Entry& entry = entries[channelId];
    entry.tracked = true;
    entry.lastViewedAt = std::max(entry.lastViewedAt, viewedAt);
    entry.readMessageCount = std::max(entry.readMessageCount, totalMessageCount);
    if (hasTotalRootMessageCount) {
        entry.readRootMessageCount = std::max(entry.readRootMessageCount, totalRootMessageCount);
        entry.hasReadRootMessageCount = true;
    }
    entry.hasUnreadActivity = false;
    entry.mentioned = false;
}

void ChannelActivityTracker::setMentioned(const QString& channelId, bool mentioned)
{
    auto it = entries.find(channelId);
    if (it == entries.end()) {
        return;
    }
    it->mentioned = mentioned;
}

void ChannelActivityTracker::setMuted(const QString& channelId, bool muted)
{
    auto it = entries.find(channelId);
    if (it == entries.end()) {
        return;
    }
    it->muted = muted;
}

bool ChannelActivityTracker::isTracked(const QString& channelId) const
{
    const auto it = entries.constFind(channelId);
    return it != entries.cend() && it->tracked;
}

bool ChannelActivityTracker::isUnread(const QString& channelId) const
{
    const auto it = entries.constFind(channelId);
    if (it == entries.cend() || !it->tracked) {
        return false;
    }

    const Entry& entry = it.value();
    return entry.mentioned || (!entry.muted && entry.hasUnreadActivity);
}

uint64_t ChannelActivityTracker::activityTime(const QString& channelId) const
{
    const auto it = entries.constFind(channelId);
    if (it == entries.cend() || !it->tracked) {
        return 0;
    }

    return std::max(it->lastViewedAt, it->lastActivityAt);
}

uint64_t ChannelActivityTracker::lastViewedTime(const QString& channelId) const
{
    const auto it = entries.constFind(channelId);
    if (it == entries.cend() || !it->tracked) {
        return 0;
    }

    return it->lastViewedAt;
}

} // namespace Mattermost
