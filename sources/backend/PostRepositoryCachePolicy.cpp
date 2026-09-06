#include "PostRepository.h"

#include <algorithm>

#include <QDateTime>
#include <QSettings>

#include "Backend.h"
#include "Settings.h"
#include "SidebarService.h"
#include "types/BackendChannel.h"

namespace Mattermost {
namespace {

constexpr qint64 MinuteMs = 60LL * 1000;
constexpr qint64 HourMs = 60LL * MinuteMs;

qint64 configuredMemoryChannelHorizonMs()
{
    return std::max<qint64>(
        MinuteMs,
        QSettings().value(POST_CACHE_MEMORY_CHANNEL_IDLE_MINUTES,
                          POST_CACHE_MEMORY_CHANNEL_IDLE_MINUTES_DEFAULT).toLongLong()
            * MinuteMs);
}

qint64 configuredDiskChannelHorizonMs()
{
    return std::max<qint64>(
        HourMs,
        QSettings().value(POST_CACHE_DISK_CHANNEL_IDLE_HOURS,
                          POST_CACHE_DISK_CHANNEL_IDLE_HOURS_DEFAULT).toLongLong()
            * HourMs);
}

QString channelInterestKey(const QString& server,
                           const QString& userId,
                           const QString& channelId)
{
    return server + QChar(0x1f) + userId + QChar(0x1f) + channelId;
}

} // namespace

void PostRepository::recordChannelOpened(const QString& channelId, quint64 openedAt)
{
    if (channelId.isEmpty()) {
        return;
    }

    const CacheAccount account = currentCacheAccount();
    if (!account.isValid()) {
        return;
    }

    const qint64 timestamp = openedAt == 0
        ? QDateTime::currentMSecsSinceEpoch()
        : static_cast<qint64>(openedAt);
    if (timestamp <= 0) {
        return;
    }

    const QString key = channelInterestKey(account.server, account.userId, channelId);
    auto it = channelOpenedAtByAccount.find(key);
    if (it == channelOpenedAtByAccount.end()) {
        channelOpenedAtByAccount.insert(key, timestamp);
    } else {
        *it = std::max(*it, timestamp);
    }

    // Persist the observation asynchronously as well. The worker applies the
    // same account key so a later login cannot steal an earlier channel visit.
    postCache.recordChannelOpened(account.server, account.userId, channelId, timestamp);
}

bool PostRepository::shouldRetainChannelInMemory(const QString& channelId) const
{
    if (channelId.isEmpty()) {
        return false;
    }

    const CacheAccount account = currentCacheAccount();
    if (!account.isValid()) {
        return false;
    }

    auto* self = const_cast<PostRepository*>(this);
    BackendChannel* const currentChannel = backend.getCurrentChannel();
    if (currentChannel && currentChannel->id == channelId) {
        // setCurrentChannel() happens before the active ChatArea starts loading
        // its source. Any cache-policy decision for that channel is therefore a
        // reliable local reading gesture even before the server echoes viewed.
        self->recordChannelOpened(channelId);
    }

    const QString key = channelInterestKey(account.server, account.userId, channelId);
    auto it = channelOpenedAtByAccount.constFind(key);
    if (it == channelOpenedAtByAccount.cend()) {
        // Mattermost persists channel_open_time as a user preference. Use it as
        // a lazy startup seed so we do not need to cache every joined channel
        // merely to reconstruct yesterday's working set.
        const quint64 serverOpenTime = SidebarService::instance(backend).channelOpenTime(channelId);
        if (serverOpenTime == 0) {
            return false;
        }
        self->recordChannelOpened(channelId, serverOpenTime);
        it = channelOpenedAtByAccount.constFind(key);
        if (it == channelOpenedAtByAccount.cend()) {
            return false;
        }
    }

    return *it >= QDateTime::currentMSecsSinceEpoch() - configuredMemoryChannelHorizonMs();
}

bool PostRepository::shouldCacheChannelOnDisk(const QString& channelId) const
{
    if (channelId.isEmpty()) {
        return false;
    }

    const CacheAccount account = currentCacheAccount();
    if (!account.isValid()) {
        return false;
    }

    auto* self = const_cast<PostRepository*>(this);
    BackendChannel* const currentChannel = backend.getCurrentChannel();
    if (currentChannel && currentChannel->id == channelId) {
        self->recordChannelOpened(channelId);
    }

    const QString key = channelInterestKey(account.server, account.userId, channelId);
    auto it = channelOpenedAtByAccount.constFind(key);
    if (it == channelOpenedAtByAccount.cend()) {
        const quint64 serverOpenTime = SidebarService::instance(backend).channelOpenTime(channelId);
        if (serverOpenTime == 0) {
            return false;
        }
        self->recordChannelOpened(channelId, serverOpenTime);
        it = channelOpenedAtByAccount.constFind(key);
        if (it == channelOpenedAtByAccount.cend()) {
            return false;
        }
    }

    return *it >= QDateTime::currentMSecsSinceEpoch() - configuredDiskChannelHorizonMs();
}

} // namespace Mattermost
