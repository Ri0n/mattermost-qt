/**
 * @file SidebarService.cpp
 * @brief Mattermost sidebar categories and per-channel mute state.
 */

#include "SidebarService.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "backend/Backend.h"
#include "backend/NetworkRequest.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendTeam.h"

namespace Mattermost {

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
}

void SidebarService::clear()
{
    httpConnector.reset();
    mutedChannelIds.clear();
    sidebarByTeam.clear();
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
        for (const auto& value : doc.array()) {
            const auto object = value.toObject();
            const auto notifyProps = object.value(QStringLiteral("notify_props")).toObject();
            if (notifyProps.value(QStringLiteral("mark_unread")).toString() == QStringLiteral("mention")) {
                mutedChannelIds.insert(object.value(QStringLiteral("channel_id")).toString());
            }
        }
        if (callback) {
            callback();
        }
    }));
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

    httpConnector.put(request, props, HttpResponseCallback([this, channelId = channel.id, muted, callback](const QJsonDocument&) {
        if (muted) {
            mutedChannelIds.insert(channelId);
        } else {
            mutedChannelIds.remove(channelId);
        }
        emit channelMutedChanged(channelId, muted);
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
    httpConnector.put(request, category.toJson(), HttpResponseCallback([this, teamId = category.teamId, callback](const QJsonDocument& doc) {
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
    httpConnector.put(request, payload, HttpResponseCallback([this, teamId, callback](const QJsonDocument& doc) {
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
    httpConnector.put(request, payload, HttpResponseCallback([this, teamId, callback](const QJsonDocument& doc) {
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
