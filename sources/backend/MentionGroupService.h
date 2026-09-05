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

#pragma once

#include <functional>

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>

#include "HTTPConnector.h"

namespace Mattermost {

class Backend;

struct MentionGroup {
    QString id;
    QString name;
    QString displayName;
    int memberCount = 0;
};

struct MentionGroupMember {
    QString id;
    QString username;
    QString displayName;
};

class MentionGroupService final : public QObject
{
    Q_OBJECT
public:
    using GroupsCallback = std::function<void()>;
    using MembersCallback = std::function<void(QVector<MentionGroupMember>)>;

    static MentionGroupService& instance(Backend& backend);

    void ensureTeamGroups(const QString& teamId, GroupsCallback callback = {});
    QHash<QString, QString> mentionIds(const QString& teamId) const;
    const MentionGroup* groupById(const QString& teamId, const QString& groupId) const;
    void retrieveMembers(const QString& groupId, MembersCallback callback);
    void clear();

signals:
    void groupsChanged(const QString& teamId);

private:
    explicit MentionGroupService(Backend& backend);
    void finishTeamLoad(const QString& teamId);

    Backend& backend;
    HTTPConnector httpConnector;
    QHash<QString, QHash<QString, MentionGroup>> groupsByTeamAndId;
    QSet<QString> loadedTeams;
    QSet<QString> loadingTeams;
    QHash<QString, QVector<GroupsCallback>> teamWaiters;
};

} // namespace Mattermost
