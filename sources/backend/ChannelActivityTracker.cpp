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

void ChannelActivityTracker::setMembership(const QString& channelId, quint64 lastViewedAt,
                                           quint64 readMessageCount, bool mentioned, bool muted)
{
    if (channelId.isEmpty()) {
        return;
    }

    Entry& entry = entries[channelId];
    entry.lastViewedAt = lastViewedAt;
    entry.readMessageCount = readMessageCount;
    entry.mentioned = mentioned;
    entry.muted = muted;
    entry.tracked = true;
}

void ChannelActivityTracker::synchronizeChannel(const QString& channelId, quint64 lastPostAt,
                                                quint64 totalMessageCount)
{
    auto it = entries.find(channelId);
    if (it == entries.end() || !it->tracked) {
        return;
    }

    Entry& entry = it.value();
    entry.lastActivityAt = std::max(entry.lastActivityAt, lastPostAt);

    const bool newerMessageCount = totalMessageCount > entry.readMessageCount;
    const bool newerTimestamp = lastPostAt > entry.lastViewedAt;
    entry.hasUnreadActivity = entry.hasUnreadActivity || newerMessageCount || newerTimestamp;
}

void ChannelActivityTracker::recordPost(const QString& channelId, quint64 createdAt, bool ownPost,
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

void ChannelActivityTracker::recordViewed(const QString& channelId, quint64 viewedAt,
                                          quint64 totalMessageCount)
{
    if (channelId.isEmpty()) {
        return;
    }

    Entry& entry = entries[channelId];
    entry.tracked = true;
    entry.lastViewedAt = std::max(entry.lastViewedAt, viewedAt);
    entry.readMessageCount = std::max(entry.readMessageCount, totalMessageCount);
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

quint64 ChannelActivityTracker::activityTime(const QString& channelId) const
{
    const auto it = entries.constFind(channelId);
    if (it == entries.cend() || !it->tracked) {
        return 0;
    }

    return std::max(it->lastViewedAt, it->lastActivityAt);
}

} // namespace Mattermost
