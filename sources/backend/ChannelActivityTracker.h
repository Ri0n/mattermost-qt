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
        uint64_t approximateViewAt = 0;
        uint64_t openTimeAt = 0;

        uint64_t readMessageCount = 0;
        uint64_t readRootMessageCount = 0;
        uint64_t mentionCount = 0;
        uint64_t rootMentionCount = 0;
        bool hasReadRootMessageCount = false;
        bool hasRootMentionCount = false;

        bool serverUnreadActivity = false;
        bool runtimeUnreadActivity = false;
        bool serverMentioned = false;
        bool runtimeMentioned = false;
        bool muted = false;
        bool tracked = false;
    };

    void clear();

    void setMembership(const QString& channelId, uint64_t lastViewedAt,
                       uint64_t readMessageCount, uint64_t readRootMessageCount,
                       bool hasReadRootMessageCount, uint64_t mentionCount,
                       uint64_t rootMentionCount, bool hasRootMentionCount, bool muted);
    void synchronizeChannel(const QString& channelId, uint64_t lastPostAt,
                            uint64_t totalMessageCount, uint64_t totalRootMessageCount,
                            bool hasTotalRootMessageCount, bool collapsedThreadsEnabled);
    void recordPost(const QString& channelId, uint64_t createdAt, bool ownPost,
                    bool threadReply, bool mentioned);
    void recordViewed(const QString& channelId, uint64_t viewedAt,
                      uint64_t totalMessageCount, uint64_t totalRootMessageCount,
                      bool hasTotalRootMessageCount);
    void setRecencyTimes(const QString& channelId, uint64_t approximateViewAt,
                         uint64_t openTimeAt);
    void setOpenTime(const QString& channelId, uint64_t openTimeAt);
    void setMentioned(const QString& channelId, bool mentioned);
    void setMuted(const QString& channelId, bool muted);

    bool isTracked(const QString& channelId) const;
    bool isUnread(const QString& channelId) const;
    bool hasMention(const QString& channelId) const;
    uint64_t activityTime(const QString& channelId) const;
    uint64_t lastViewedTime(const QString& channelId) const;
    uint64_t recentTime(const QString& channelId) const;

    /** Raw Mattermost channel_open_time preference; message traffic never changes it. */
    uint64_t openTime(const QString& channelId) const
    {
        const auto it = entries.constFind(channelId);
        return it == entries.cend() ? 0 : it->openTimeAt;
    }

private:
    QHash<QString, Entry> entries;
    bool collapsedThreadsEnabled = false;
};

} // namespace Mattermost
