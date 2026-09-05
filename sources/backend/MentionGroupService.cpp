/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "MentionGroupService.h"

#include <algorithm>
#include <memory>

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>

#include "Backend.h"
#include "HttpResponseCallback.h"
#include "NetworkRequest.h"

namespace Mattermost {
namespace {

constexpr int GroupMembersPerPage = 100;

QString displayName(const QJsonObject& user)
{
    const QString firstName = user.value(QStringLiteral("first_name")).toString();
    const QString lastName = user.value(QStringLiteral("last_name")).toString();
    if (!firstName.isEmpty()) {
        return lastName.isEmpty() ? firstName : firstName + QLatin1Char(' ') + lastName;
    }
    return user.value(QStringLiteral("username")).toString();
}

} // namespace

MentionGroupService& MentionGroupService::instance(Backend& backend)
{
    static QHash<Backend*, QPointer<MentionGroupService>> instances;
    QPointer<MentionGroupService>& service = instances[&backend];
    if (!service) {
        service = new MentionGroupService(backend);
    }
    return *service;
}

MentionGroupService::MentionGroupService(Backend& sourceBackend)
    : QObject(&sourceBackend)
    , backend(sourceBackend)
{
}

void MentionGroupService::clear()
{
    groupsByTeamAndId.clear();
    loadedTeams.clear();
    loadingTeams.clear();
    teamWaiters.clear();
}

void MentionGroupService::ensureTeamGroups(const QString& teamId, GroupsCallback callback)
{
    if (teamId.isEmpty()) {
        if (callback) {
            callback();
        }
        return;
    }

    if (loadedTeams.contains(teamId)) {
        if (callback) {
            callback();
        }
        return;
    }

    if (callback) {
        teamWaiters[teamId].push_back(std::move(callback));
    }
    if (loadingTeams.contains(teamId)) {
        return;
    }
    loadingTeams.insert(teamId);

    NetworkRequest request(
        QStringLiteral("teams/") + teamId
        + QStringLiteral("/groups?paginate=false&filter_allow_reference=true&include_member_count=true"));
    httpConnector.get(request, HttpResponseCallback(
        [this, teamId](const QJsonDocument& doc) {
            QHash<QString, MentionGroup> groups;
            const QJsonArray array = doc.object().value(QStringLiteral("groups")).toArray();
            for (const QJsonValue& value : array) {
                const QJsonObject object = value.toObject();
                MentionGroup group;
                group.id = object.value(QStringLiteral("id")).toString();
                group.name = object.value(QStringLiteral("name")).toString();
                group.displayName = object.value(QStringLiteral("display_name")).toString();
                group.memberCount = object.value(QStringLiteral("member_count")).toInt();
                if (group.id.isEmpty() || group.name.isEmpty()
                    || !object.value(QStringLiteral("allow_reference")).toBool(true)) {
                    continue;
                }
                groups.insert(group.id, std::move(group));
            }
            groupsByTeamAndId.insert(teamId, std::move(groups));
            finishTeamLoad(teamId);
        }));
}

void MentionGroupService::finishTeamLoad(const QString& teamId)
{
    loadingTeams.remove(teamId);
    loadedTeams.insert(teamId);

    QVector<GroupsCallback> callbacks = teamWaiters.take(teamId);
    emit groupsChanged(teamId);
    for (auto& callback : callbacks) {
        if (callback) {
            callback();
        }
    }
}

QHash<QString, QString> MentionGroupService::mentionIds(const QString& teamId) const
{
    QHash<QString, QString> result;
    const auto teamIt = groupsByTeamAndId.constFind(teamId);
    if (teamIt == groupsByTeamAndId.cend()) {
        return result;
    }

    for (auto it = teamIt->cbegin(); it != teamIt->cend(); ++it) {
        result.insert(it->name.toLower(), it->id);
    }
    return result;
}

const MentionGroup* MentionGroupService::groupById(const QString& teamId,
                                                   const QString& groupId) const
{
    const auto teamIt = groupsByTeamAndId.constFind(teamId);
    if (teamIt == groupsByTeamAndId.cend()) {
        return nullptr;
    }
    const auto groupIt = teamIt->constFind(groupId);
    return groupIt == teamIt->cend() ? nullptr : &groupIt.value();
}

void MentionGroupService::retrieveMembers(const QString& groupId, MembersCallback callback)
{
    if (groupId.isEmpty()) {
        if (callback) {
            callback({});
        }
        return;
    }

    auto members = std::make_shared<QVector<MentionGroupMember>>();
    auto fetchPage = std::make_shared<std::function<void(int)>>();
    *fetchPage = [this, groupId, callback = std::move(callback), members, fetchPage](int page) mutable {
        NetworkRequest request(
            QStringLiteral("users?in_group=") + groupId
            + QStringLiteral("&page=") + QString::number(page)
            + QStringLiteral("&per_page=") + QString::number(GroupMembersPerPage));

        httpConnector.get(request, HttpResponseCallback(
            [callback, members, fetchPage, page](const QJsonDocument& doc) mutable {
                const QJsonArray array = doc.array();
                members->reserve(members->size() + array.size());
                for (const QJsonValue& value : array) {
                    const QJsonObject object = value.toObject();
                    MentionGroupMember member;
                    member.id = object.value(QStringLiteral("id")).toString();
                    member.username = object.value(QStringLiteral("username")).toString();
                    member.displayName = displayName(object);
                    if (!member.id.isEmpty()) {
                        members->push_back(std::move(member));
                    }
                }

                if (array.size() == GroupMembersPerPage) {
                    (*fetchPage)(page + 1);
                    return;
                }

                std::sort(members->begin(), members->end(),
                          [](const MentionGroupMember& lhs, const MentionGroupMember& rhs) {
                    return lhs.displayName.compare(rhs.displayName, Qt::CaseInsensitive) < 0;
                });
                if (callback) {
                    callback(std::move(*members));
                }
            }));
    };

    (*fetchPage)(0);
}

} // namespace Mattermost
