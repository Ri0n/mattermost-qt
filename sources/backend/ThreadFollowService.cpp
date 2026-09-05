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

void ThreadFollowService::queryThread(const QString& teamId,
                                      const QString& threadId,
                                      ThreadStateCallback callback)
{
    if (teamId.isEmpty() || threadId.isEmpty() || backend.getLoginUser().id.isEmpty()) {
        if (callback) {
            callback(ThreadState {});
        }
        return;
    }

    // This endpoint returns the current user's ThreadResponse, not merely a
    // boolean following flag. Keep its read metadata so navigation can use the
    // same last_viewed_at boundary as the Mattermost web client.
    NetworkRequest request(threadPath(teamId, threadId));
    httpConnector.get(request, HttpResponseCallback(
        [callback = std::move(callback)](QVariant status, const QJsonDocument& doc) mutable {
            ThreadState state;
            state.available = status.toInt() == QNetworkReply::NoError && doc.isObject();
            if (state.available) {
                const QJsonObject object = doc.object();
                state.lastViewedAt = object.value(QStringLiteral("last_viewed_at"))
                    .toVariant().toULongLong();
                state.lastReplyAt = object.value(QStringLiteral("last_reply_at"))
                    .toVariant().toULongLong();
                state.unreadReplies = object.value(QStringLiteral("unread_replies")).toInt();
                state.unreadMentions = object.value(QStringLiteral("unread_mentions")).toInt();
            }
            if (callback) {
                callback(state);
            }
        }));
}

void ThreadFollowService::queryFollowing(const QString& teamId,
                                         const QString& threadId,
                                         std::function<void(bool)> callback)
{
    queryThread(teamId, threadId,
                [callback = std::move(callback)](const ThreadState& state) mutable {
        if (callback) {
            callback(state.available);
        }
    });
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
