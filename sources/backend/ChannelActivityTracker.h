/**
 * @file ChannelActivityTracker.h
 * @brief Tracks recent and unread channel activity independently of the UI.
 */

#pragma once

#include <cstdint>

#include <QHash>
#include <QString>

namespace Mattermost {

class ChannelActivityTracker
{
public:
    struct Entry {
        uint64_t lastViewedAt = 0;
        uint64_t lastActivityAt = 0;
        uint64_t readRootMessageCount = 0;
        bool hasUnreadActivity = false;
        bool mentioned = false;
        bool muted = false;
        bool tracked = false;
    };

    void clear();

    void setMembership(const QString& channelId, uint64_t lastViewedAt,
                       uint64_t readRootMessageCount, bool mentioned, bool muted);
    void synchronizeChannel(const QString& channelId, uint64_t lastPostAt,
                            uint64_t totalRootMessageCount);
    void recordPost(const QString& channelId, uint64_t createdAt, bool ownPost,
                    bool threadReply, bool mentioned);
    void recordViewed(const QString& channelId, uint64_t viewedAt,
                      uint64_t totalRootMessageCount);
    void setMentioned(const QString& channelId, bool mentioned);
    void setMuted(const QString& channelId, bool muted);

    bool isTracked(const QString& channelId) const;
    bool isUnread(const QString& channelId) const;
    uint64_t activityTime(const QString& channelId) const;

private:
    QHash<QString, Entry> entries;
};

} // namespace Mattermost
