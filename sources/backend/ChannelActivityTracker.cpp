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
                                           bool hasReadRootMessageCount, uint64_t mentionCount,
                                           uint64_t rootMentionCount, bool hasRootMentionCount,
                                           bool muted)
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

    entry.mentionCount = mentionCount;
    entry.rootMentionCount = rootMentionCount;
    entry.hasRootMentionCount = hasRootMentionCount;
    entry.muted = muted;
    entry.tracked = true;
}

void ChannelActivityTracker::synchronizeChannel(const QString& channelId, uint64_t lastPostAt,
                                                uint64_t totalMessageCount,
                                                uint64_t totalRootMessageCount,
                                                bool hasTotalRootMessageCount,
                                                bool collapsedThreadsEnabled)
{
    auto it = entries.find(channelId);
    if (it == entries.end() || !it->tracked) {
        return;
    }

    Entry& entry = it.value();
    entry.lastActivityAt = std::max(entry.lastActivityAt, lastPostAt);

    const bool useRootCounts = collapsedThreadsEnabled
        && entry.hasReadRootMessageCount
        && hasTotalRootMessageCount;
    const uint64_t readCount = useRootCounts ? entry.readRootMessageCount : entry.readMessageCount;
    const uint64_t totalCount = useRootCounts ? totalRootMessageCount : totalMessageCount;

    entry.serverUnreadActivity = totalCount > readCount;
    entry.serverMentioned = collapsedThreadsEnabled && entry.hasRootMentionCount
        ? entry.rootMentionCount > 0
        : entry.mentionCount > 0;
}

void ChannelActivityTracker::recordPost(const QString& channelId, uint64_t createdAt, bool ownPost,
                                        bool threadReply, bool mentioned,
                                        bool collapsedThreadsEnabled)
{
    if (channelId.isEmpty()) {
        return;
    }

    Entry& entry = entries[channelId];
    entry.tracked = true;
    entry.lastActivityAt = std::max(entry.lastActivityAt, createdAt);

    // With CRT enabled, reply activity and mentions belong to the followed
    // thread model, not to the parent channel's root counters. With CRT off,
    // replies are ordinary channel activity and use the normal counters.
    const bool belongsToParentChannel = !threadReply || !collapsedThreadsEnabled;
    if (mentioned && belongsToParentChannel) {
        entry.runtimeMentioned = true;
    }

    if (ownPost) {
        return;
    }

    if (belongsToParentChannel) {
        entry.runtimeUnreadActivity = true;
    }
}

void ChannelActivityTracker::recordViewed(const QString& channelId, uint64_t viewedAt,
                                          uint64_t totalMessageCount,
                                          uint64_t totalRootMessageCount,
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

    entry.mentionCount = 0;
    entry.rootMentionCount = 0;
    entry.serverUnreadActivity = false;
    entry.runtimeUnreadActivity = false;
    entry.serverMentioned = false;
    entry.runtimeMentioned = false;
}

void ChannelActivityTracker::setRecencyTimes(const QString& channelId, uint64_t approximateViewAt,
                                             uint64_t openTimeAt)
{
    if (channelId.isEmpty()) {
        return;
    }

    Entry& entry = entries[channelId];
    entry.approximateViewAt = std::max(entry.approximateViewAt, approximateViewAt);
    entry.openTimeAt = std::max(entry.openTimeAt, openTimeAt);
}

void ChannelActivityTracker::setOpenTime(const QString& channelId, uint64_t openTimeAt)
{
    if (channelId.isEmpty()) {
        return;
    }
    entries[channelId].openTimeAt = std::max(entries[channelId].openTimeAt, openTimeAt);
}

void ChannelActivityTracker::setMentioned(const QString& channelId, bool mentioned)
{
    auto it = entries.find(channelId);
    if (it == entries.end()) {
        return;
    }

    if (mentioned) {
        it->runtimeMentioned = true;
    } else {
        it->mentionCount = 0;
        it->rootMentionCount = 0;
        it->serverMentioned = false;
        it->runtimeMentioned = false;
    }
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
    const bool mentioned = entry.serverMentioned || entry.runtimeMentioned;
    const bool unreadActivity = entry.serverUnreadActivity || entry.runtimeUnreadActivity;
    return mentioned || (!entry.muted && unreadActivity);
}

bool ChannelActivityTracker::hasMention(const QString& channelId) const
{
    const auto it = entries.constFind(channelId);
    return it != entries.cend() && (it->serverMentioned || it->runtimeMentioned);
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

uint64_t ChannelActivityTracker::recentTime(const QString& channelId) const
{
    const auto it = entries.constFind(channelId);
    if (it == entries.cend() || !it->tracked) {
        return 0;
    }

    return std::max({it->lastViewedAt, it->approximateViewAt, it->openTimeAt});
}

} // namespace Mattermost
