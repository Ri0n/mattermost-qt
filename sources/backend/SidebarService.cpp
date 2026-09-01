/**
 * @file SidebarService.cpp
 * @brief Mattermost sidebar categories and per-channel notification state.
 */

#include "SidebarService.h"

#include <algorithm>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "backend/Backend.h"
#include "backend/NetworkRequest.h"
#include "backend/QByteArrayCreator.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "backend/types/BackendTeam.h"

namespace Mattermost {

namespace {

const QString channelOpenTimeCategory = QStringLiteral("channel_open_time");
const QString channelApproximateViewTimeCategory = QStringLiteral("channel_approximate_view_time");
const QString displaySettingsCategory = QStringLiteral("display_settings");
const QString collapsedReplyThreadsName = QStringLiteral("collapsed_reply_threads");

} // namespace

SidebarCategory SidebarCategory::fromJson(const QJsonObject& object)
{
    SidebarCategory category;
    category.id = object.value(QStringLiteral("id")).toString();
    category.userId = object.value(QStringLiteral("user_id")).toString();
    category.teamId = object.value(QStringLiteral("team_id")).toString();
    category.sortOrder = object.value(QStringLiteral("sort_order")).toVariant().toLongLong();
    category.sorting = object.value(QStringLiteral("sorting")).toString();
    category.type = object.value(QStringLiteral("type")).toString();
    category.displayName = object.value(QStringLiteral("display_name")).toString();
    category.muted = object.value(QStringLiteral("muted")).toBool();
    category.collapsed = object.value(QStringLiteral("collapsed")).toBool();

    for (const auto& channelId : object.value(QStringLiteral("channel_ids")).toArray()) {
        category.channelIds.push_back(channelId.toString());
    }

    return category;
}

QJsonObject SidebarCategory::toJson() const
{
    QJsonArray channels;
    for (const auto& channelId : channelIds) {
        channels.push_back(channelId);
    }

    return QJsonObject {
        {QStringLiteral("id"), id},
        {QStringLiteral("user_id"), userId},
        {QStringLiteral("team_id"), teamId},
        {QStringLiteral("sort_order"), static_cast<double>(sortOrder)},
        {QStringLiteral("sorting"), sorting},
        {QStringLiteral("type"), type},
        {QStringLiteral("display_name"), displayName},
        {QStringLiteral("muted"), muted},
        {QStringLiteral("collapsed"), collapsed},
        {QStringLiteral("channel_ids"), channels},
    };
}

SidebarCategory* SidebarTeamState::category(const QString& categoryId)
{
    auto it = categories.find(categoryId);
    return it == categories.end() ? nullptr : &it.value();
}

const SidebarCategory* SidebarTeamState::category(const QString& categoryId) const
{
    auto it = categories.constFind(categoryId);
    return it == categories.cend() ? nullptr : &it.value();
}

SidebarCategory* SidebarTeamState::categoryByType(const QString& type)
{
    for (auto it = categories.begin(); it != categories.end(); ++it) {
        if (it->type == type) {
            return &it.value();
        }
    }
    return nullptr;
}

const SidebarCategory* SidebarTeamState::categoryByType(const QString& type) const
{
    for (auto it = categories.cbegin(); it != categories.cend(); ++it) {
        if (it->type == type) {
            return &it.value();
        }
    }
    return nullptr;
}

SidebarService& SidebarService::instance(Backend& backend)
{
    static QMap<Backend*, SidebarService*> instances;
    auto it = instances.find(&backend);
    if (it == instances.end()) {
        it = instances.insert(&backend, new SidebarService(backend));
    }
    return **it;
}

SidebarService::SidebarService(Backend& backend)
    : QObject(&backend)
    , backend(backend)
{
    connect(&httpConnector, &HTTPConnector::onNetworkError,
            &backend, &Backend::onNetworkError);
    connect(&httpConnector, &HTTPConnector::onHttpError,
            &backend, &Backend::onHttpError);

    connect(&backend, &Backend::onNewPost, this,
            [this](BackendChannel& channel, const BackendPost& post) {
        recordChannelPost(channel, post);
    });
    connect(&backend, &Backend::onChannelViewed, this,
            [this](const BackendChannel& channel) {
        recordChannelViewed(channel);
    });
    connect(&backend, &Backend::onAllTeamChannelsPopulated,
            this, &SidebarService::synchronizeChannelActivity);
}

void SidebarService::clear()
{
    httpConnector.reset();
    mutedChannelIds.clear();
    sidebarByTeam.clear();
    activityTracker.clear();
    collapsedThreadsConfig.clear();
    hasCollapsedThreadsPreference = false;
    collapsedThreadsPreference = false;
    collapsedThreadsEnabled = false;
    emit channelActivityReset();
}

QString SidebarService::currentUserId() const
{
    return backend.getLoginUser().id;
}

QString SidebarService::categoriesPath(const QString& teamId) const
{
    return QStringLiteral("users/") + currentUserId() + QStringLiteral("/teams/") + teamId
        + QStringLiteral("/channels/categories");
}

bool SidebarService::isChannelMuted(const BackendChannel& channel) const
{
    return isChannelMuted(channel.id);
}

bool SidebarService::isChannelMuted(const QString& channelId) const
{
    return mutedChannelIds.contains(channelId);
}

bool SidebarService::hasUnreadMention(const QString& channelId) const
{
    return activityTracker.hasMention(channelId);
}

void SidebarService::setChannelMentioned(const QString& channelId, bool mentioned)
{
    if (channelId.isEmpty()) {
        return;
    }

    const bool wasMentioned = activityTracker.hasMention(channelId);
    activityTracker.setMentioned(channelId, mentioned);
    const bool isMentioned = activityTracker.hasMention(channelId);
    if (wasMentioned == isMentioned) {
        return;
    }

    emit channelMentionedChanged(channelId, isMentioned);
    emit channelActivityChanged(channelId);
}

bool SidebarService::isChannelTracked(const QString& channelId) const
{
    if (!activityTracker.isTracked(channelId)) {
        return false;
    }

    // Keep Recent/Unreads in the same universe as the Channels tab. A channel
    // membership may exist on the server while the conversation is hidden by
    // the user's sidebar configuration (for example an old DM beyond the
    // configured Direct Messages limit).
    for (auto teamIt = sidebarByTeam.cbegin(); teamIt != sidebarByTeam.cend(); ++teamIt) {
        const SidebarTeamState& state = teamIt.value();
        for (auto categoryIt = state.categories.cbegin(); categoryIt != state.categories.cend(); ++categoryIt) {
            if (categoryIt->channelIds.contains(channelId)) {
                return true;
            }
        }
    }
    return false;
}

bool SidebarService::isChannelUnread(const BackendChannel& channel) const
{
    return activityTracker.isUnread(channel.id);
}

uint64_t SidebarService::channelActivityTime(const BackendChannel& channel) const
{
    return std::max(activityTracker.activityTime(channel.id), channel.last_post_at);
}

uint64_t SidebarService::channelRecentTime(const BackendChannel& channel) const
{
    return activityTracker.recentTime(channel.id);
}

void SidebarService::markChannelViewedLocally(const BackendChannel& channel)
{
    recordChannelViewed(channel);
}

void SidebarService::synchronizeChannelActivity()
{
    const auto& channels = backend.getStorage().channels;
    for (auto it = channels.cbegin(); it != channels.cend(); ++it) {
        const BackendChannel* channel = it.value();
        if (!channel) {
            continue;
        }
        activityTracker.synchronizeChannel(
            channel->id,
            channel->last_post_at,
            static_cast<uint64_t>(std::max(0, channel->total_msg_count)),
            static_cast<uint64_t>(std::max(0, channel->total_msg_count_root)),
            channel->has_total_msg_count_root,
            collapsedThreadsEnabled);
    }
    emit channelActivityReset();
}

void SidebarService::recordChannelPost(BackendChannel& channel, const BackendPost& post)
{
    activityTracker.recordPost(channel.id, post.create_at, post.isOwnPost(),
                               !post.root_id.isEmpty(), post.currentUserMentioned);
    emit channelActivityChanged(channel.id);
}

void SidebarService::recordChannelViewed(const BackendChannel& channel)
{
    const bool wasMentioned = activityTracker.hasMention(channel.id);

    // Mattermost's server-side last_viewed_at is tied to channel content, not
    // wall-clock click history. Reopening an already-read channel therefore
    // leaves its Recent ordering unchanged until newer content is consumed.
    activityTracker.recordViewed(
        channel.id,
        channel.last_post_at,
        static_cast<uint64_t>(std::max(0, channel.total_msg_count)),
        static_cast<uint64_t>(std::max(0, channel.total_msg_count_root)),
        channel.has_total_msg_count_root);

    if (wasMentioned) {
        emit channelMentionedChanged(channel.id, false);
    }
    emit channelActivityChanged(channel.id);
}

void SidebarService::retrieveChannelMemberships(std::function<void()> callback)
{
    if (currentUserId().isEmpty()) {
        if (callback) {
            callback();
        }
        return;
    }

    NetworkRequest request(QStringLiteral("users/") + currentUserId() + QStringLiteral("/channel_members"));
    httpConnector.get(request, HttpResponseCallback([this, callback](const QJsonDocument& doc) {
        mutedChannelIds.clear();

        // Do not clear the tracker here. A websocket post may have arrived while
        // this membership request was in flight. Server-derived and runtime
        // unread state are kept separately so this response can refresh one
        // without destroying the other.
        for (const auto& value : doc.array()) {
            const auto object = value.toObject();
            const QString channelId = object.value(QStringLiteral("channel_id")).toString();
            const auto notifyProps = object.value(QStringLiteral("notify_props")).toObject();
            const bool muted = notifyProps.value(QStringLiteral("mark_unread")).toString()
                == QStringLiteral("mention");
            const uint64_t readMessageCount = object.value(QStringLiteral("msg_count"))
                .toVariant().toULongLong();
            const bool hasReadRootMessageCount = object.contains(QStringLiteral("msg_count_root"));
            const uint64_t readRootMessageCount = hasReadRootMessageCount
                ? object.value(QStringLiteral("msg_count_root")).toVariant().toULongLong()
                : 0;
            const uint64_t mentionCount = object.value(QStringLiteral("mention_count"))
                .toVariant().toULongLong();
            const bool hasRootMentionCount = object.contains(QStringLiteral("mention_count_root"));
            const uint64_t rootMentionCount = hasRootMentionCount
                ? object.value(QStringLiteral("mention_count_root")).toVariant().toULongLong()
                : 0;

            if (muted) {
                mutedChannelIds.insert(channelId);
            }

            activityTracker.setMembership(
                channelId,
                object.value(QStringLiteral("last_viewed_at")).toVariant().toULongLong(),
                readMessageCount,
                readRootMessageCount,
                hasReadRootMessageCount,
                mentionCount,
                rootMentionCount,
                hasRootMentionCount,
                muted);
        }

        synchronizeChannelActivity();
        if (callback) {
            callback();
        }
    }));
}

void SidebarService::retrieveChannelPreferences(std::function<void()> callback)
{
    if (currentUserId().isEmpty()) {
        if (callback) {
            callback();
        }
        return;
    }

    NetworkRequest request(QStringLiteral("users/") + currentUserId() + QStringLiteral("/preferences"));
    httpConnector.get(request, HttpResponseCallback([this, callback](const QJsonDocument& doc) {
        QMap<QString, uint64_t> approximateViewTimes;
        QMap<QString, uint64_t> openTimes;
        hasCollapsedThreadsPreference = false;

        for (const auto& value : doc.array()) {
            const QJsonObject object = value.toObject();
            const QString category = object.value(QStringLiteral("category")).toString();
            const QString name = object.value(QStringLiteral("name")).toString();
            const QString preferenceValue = object.value(QStringLiteral("value")).toString();

            if (category == channelApproximateViewTimeCategory) {
                approximateViewTimes.insert(name, preferenceValue.toULongLong());
            } else if (category == channelOpenTimeCategory) {
                openTimes.insert(name, preferenceValue.toULongLong());
            } else if (category == displaySettingsCategory && name == collapsedReplyThreadsName) {
                hasCollapsedThreadsPreference = true;
                collapsedThreadsPreference = preferenceValue == QStringLiteral("on");
            }
        }

        QSet<QString> channelIds;
        for (auto it = approximateViewTimes.cbegin(); it != approximateViewTimes.cend(); ++it) {
            channelIds.insert(it.key());
        }
        for (auto it = openTimes.cbegin(); it != openTimes.cend(); ++it) {
            channelIds.insert(it.key());
        }
        for (const QString& channelId : channelIds) {
            activityTracker.setRecencyTimes(
                channelId,
                approximateViewTimes.value(channelId, 0),
                openTimes.value(channelId, 0));
        }

        updateCollapsedThreadsMode();
        emit channelActivityReset();
        if (callback) {
            callback();
        }
    }));
}

void SidebarService::retrieveClientConfig(std::function<void()> callback)
{
    NetworkRequest request(QStringLiteral("config/client?format=old"));
    httpConnector.get(request, HttpResponseCallback([this, callback](const QJsonDocument& doc) {
        collapsedThreadsConfig = doc.object().value(QStringLiteral("CollapsedThreads")).toString();
        updateCollapsedThreadsMode();
        if (callback) {
            callback();
        }
    }));
}

void SidebarService::updateCollapsedThreadsMode()
{
    bool enabled = false;
    if (collapsedThreadsConfig == QStringLiteral("always_on")) {
        enabled = true;
    } else if (collapsedThreadsConfig == QStringLiteral("disabled")) {
        enabled = false;
    } else if (hasCollapsedThreadsPreference) {
        enabled = collapsedThreadsPreference;
    } else {
        enabled = collapsedThreadsConfig == QStringLiteral("default_on");
    }

    if (collapsedThreadsEnabled == enabled) {
        return;
    }

    collapsedThreadsEnabled = enabled;
    synchronizeChannelActivity();
}

void SidebarService::setChannelMuted(BackendChannel& channel, bool muted,
                                     std::function<void(bool)> callback)
{
    if (currentUserId().isEmpty()) {
        if (callback) {
            callback(false);
        }
        return;
    }

    NetworkRequest request(QStringLiteral("channels/") + channel.id + QStringLiteral("/members/")
                           + currentUserId() + QStringLiteral("/notify_props"));
    const QJsonObject props {
        {QStringLiteral("mark_unread"), muted ? QStringLiteral("mention") : QStringLiteral("all")},
    };

    httpConnector.put(request, QByteArrayCreator(props),
                      HttpResponseCallback([this, channelId = channel.id, muted, callback](const QJsonDocument&) {
        if (muted) {
            mutedChannelIds.insert(channelId);
        } else {
            mutedChannelIds.remove(channelId);
        }
        activityTracker.setMuted(channelId, muted);
        emit channelMutedChanged(channelId, muted);
        emit channelActivityChanged(channelId);
        if (callback) {
            callback(true);
        }
    }));
}

void SidebarService::retrieveCategories(BackendTeam& team,
                                        std::function<void(const SidebarTeamState&)> callback)
{
    NetworkRequest request(categoriesPath(team.id));
    httpConnector.get(request, HttpResponseCallback([this, teamId = team.id, callback](const QJsonDocument& doc) {
        SidebarTeamState state;
        const auto object = doc.object();

        for (const auto& categoryValue : object.value(QStringLiteral("categories")).toArray()) {
            SidebarCategory category = SidebarCategory::fromJson(categoryValue.toObject());
            state.categories.insert(category.id, std::move(category));
        }
        for (const auto& categoryId : object.value(QStringLiteral("order")).toArray()) {
            state.order.push_back(categoryId.toString());
        }

        for (auto it = state.categories.cbegin(); it != state.categories.cend(); ++it) {
            if (!state.order.contains(it.key())) {
                state.order.push_back(it.key());
            }
        }

        sidebarByTeam.insert(teamId, std::move(state));
        emit categoriesChanged(teamId);
        if (callback) {
            callback(sidebarByTeam[teamId]);
        }
    }));
}

const SidebarTeamState* SidebarService::teamState(const QString& teamId) const
{
    auto it = sidebarByTeam.constFind(teamId);
    return it == sidebarByTeam.cend() ? nullptr : &it.value();
}

SidebarTeamState* SidebarService::teamState(const QString& teamId)
{
    auto it = sidebarByTeam.find(teamId);
    return it == sidebarByTeam.end() ? nullptr : &it.value();
}

void SidebarService::updateCategory(const SidebarCategory& category,
                                    std::function<void(const SidebarCategory&)> callback)
{
    NetworkRequest request(categoriesPath(category.teamId) + QLatin1Char('/') + category.id);
    httpConnector.put(request, QByteArrayCreator(category.toJson()),
                      HttpResponseCallback([this, teamId = category.teamId, callback](const QJsonDocument& doc) {
        SidebarCategory updated = SidebarCategory::fromJson(doc.object());
        sidebarByTeam[teamId].categories.insert(updated.id, updated);
        emit categoriesChanged(teamId);
        if (callback) {
            callback(sidebarByTeam[teamId].categories[updated.id]);
        }
    }));
}

void SidebarService::updateCategories(const QString& teamId, const QVector<SidebarCategory>& categories,
                                      std::function<void(const SidebarTeamState&)> callback)
{
    QJsonArray payload;
    for (const auto& category : categories) {
        payload.push_back(category.toJson());
    }

    NetworkRequest request(categoriesPath(teamId));
    httpConnector.put(request, QByteArrayCreator(payload),
                      HttpResponseCallback([this, teamId, callback](const QJsonDocument& doc) {
        for (const auto& value : doc.array()) {
            SidebarCategory updated = SidebarCategory::fromJson(value.toObject());
            sidebarByTeam[teamId].categories.insert(updated.id, std::move(updated));
        }
        emit categoriesChanged(teamId);
        if (callback) {
            callback(sidebarByTeam[teamId]);
        }
    }));
}

void SidebarService::updateCategoryOrder(const QString& teamId, const QStringList& order,
                                         std::function<void()> callback)
{
    QJsonArray payload;
    for (const auto& categoryId : order) {
        payload.push_back(categoryId);
    }

    NetworkRequest request(categoriesPath(teamId) + QStringLiteral("/order"));
    httpConnector.put(request, QByteArrayCreator(payload),
                      HttpResponseCallback([this, teamId, callback](const QJsonDocument& doc) {
        QStringList updatedOrder;
        for (const auto& value : doc.array()) {
            updatedOrder.push_back(value.toString());
        }
        sidebarByTeam[teamId].order = updatedOrder;
        emit categoriesChanged(teamId);
        if (callback) {
            callback();
        }
    }));
}

} // namespace Mattermost
