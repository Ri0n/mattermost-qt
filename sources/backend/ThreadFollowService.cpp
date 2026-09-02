/**
 * @file ThreadFollowService.cpp
 * @brief Follow/unfollow state for Mattermost CRT threads.
 *
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include "ThreadFollowService.h"

#include <QHash>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QPointer>

#include "Backend.h"
#include "NetworkRequest.h"
#include "QByteArrayCreator.h"

namespace Mattermost {

ThreadFollowService& ThreadFollowService::instance(Backend& backend)
{
    static QHash<Backend*, QPointer<ThreadFollowService>> instances;
    QPointer<ThreadFollowService>& service = instances[&backend];
    if (!service) {
        service = new ThreadFollowService(backend);
    }
    return *service;
}

ThreadFollowService::ThreadFollowService(Backend& sourceBackend)
    : QObject(&sourceBackend)
    , backend(sourceBackend)
{
    // A GET for an unfollowed thread is expected to fail because the server's
    // GetThreadForUser path only exposes following memberships. Keep those
    // expected 4xx responses local instead of surfacing them as global errors.
}

QString ThreadFollowService::threadPath(const QString& teamId, const QString& threadId) const
{
    return QStringLiteral("users/") + backend.getLoginUser().id
        + QStringLiteral("/teams/") + teamId
        + QStringLiteral("/threads/") + threadId;
}

void ThreadFollowService::queryFollowing(const QString& teamId,
                                         const QString& threadId,
                                         std::function<void(bool)> callback)
{
    if (teamId.isEmpty() || threadId.isEmpty() || backend.getLoginUser().id.isEmpty()) {
        if (callback) {
            callback(false);
        }
        return;
    }

    NetworkRequest request(threadPath(teamId, threadId));
    httpConnector.get(request, HttpResponseCallback(
        [callback = std::move(callback)](QVariant status, const QJsonDocument&) mutable {
            if (callback) {
                // Mattermost returns this user-thread only for a following
                // membership. Therefore success itself is the state probe.
                callback(status.toInt() == QNetworkReply::NoError);
            }
        }));
}

void ThreadFollowService::setFollowing(const QString& teamId,
                                       const QString& threadId,
                                       bool following,
                                       std::function<void(bool)> callback)
{
    if (teamId.isEmpty() || threadId.isEmpty() || backend.getLoginUser().id.isEmpty()) {
        if (callback) {
            callback(false);
        }
        return;
    }

    NetworkRequest request(threadPath(teamId, threadId) + QStringLiteral("/following"));
    if (!following) {
        // HTTPConnector's DELETE API is intentionally fire-and-forget. Update
        // the UI optimistically; the next query/Attention refresh reconciles it.
        httpConnector.del(request);
        emit followingChanged(teamId, threadId, false);
        if (callback) {
            callback(true);
        }
        return;
    }

    httpConnector.put(request, QByteArrayCreator(QJsonObject {}),
                      HttpResponseCallback([this, teamId, threadId, callback = std::move(callback)](
                                               QVariant status, const QJsonDocument&) mutable {
        const bool success = status.toInt() == QNetworkReply::NoError;
        if (success) {
            emit followingChanged(teamId, threadId, true);
        }
        if (callback) {
            callback(success);
        }
    }));
}

} // namespace Mattermost
