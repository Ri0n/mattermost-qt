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
                                           uint64_t readRootMessageCount, bool mentioned, bool muted)
{
    if (channelId.isEmpty()) {
        return;
    }

    Entry& entry = entries[channelId];
    entry.lastViewedAt = std::max(entry.lastViewedAt, lastViewedAt);
    entry.readRootMessageCount = std::max(entry.readRootMessageCount, readRootMessageCount);
    entry.mentioned = entry.mentioned || mentioned;
    entry.muted = muted;
    entry.tracked = true;
}

void ChannelActivityTracker::synchronizeChannel(const QString& channelId, uint64_t lastPostAt,
                                                uint64_t totalRootMessageCount)
{
    auto it = entries.find(channelId);
    if (it == entries.end() || !it->tracked) {
        return;
    }

    Entry& entry = it.value();
    entry.lastActivityAt = std::max(entry.lastActivityAt, lastPostAt);

    // Mattermost keeps root-post counts separately from replies. Use the root
    // count to decide whether the parent channel has unread activity, while
    // last_post_at still makes thread replies affect Recent ordering.
    entry.hasUnreadActivity = entry.hasUnreadActivity
        || totalRootMessageCount > entry.readRootMessageCount;
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
                                          uint64_t totalRootMessageCount)
{
    if (channelId.isEmpty()) {
        return;
    }

    Entry& entry = entries[channelId];
    entry.tracked = true;
    entry.lastViewedAt = std::max(entry.lastViewedAt, viewedAt);
    entry.readRootMessageCount = std::max(entry.readRootMessageCount, totalRootMessageCount);
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

} // namespace Mattermost
