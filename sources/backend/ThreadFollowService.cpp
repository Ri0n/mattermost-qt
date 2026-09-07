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

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QPointer>

#include "Backend.h"
#include "NetworkRequest.h"
#include "QByteArrayCreator.h"
#include "Storage.h"

namespace Mattermost {
namespace {

constexpr int ThreadsPerPage = 100;

} // namespace

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

void ThreadFollowService::queryUnreadThreads(ThreadListCallback callback)
{
    if (backend.getLoginUser().id.isEmpty()) {
        if (callback) {
            callback({});
        }
        return;
    }

    auto teamIds = std::make_shared<QStringList>();
    for (const auto& pair : backend.getStorage().teams) {
        if (!pair.first.isEmpty()) {
            teamIds->push_back(pair.first);
        }
    }

    if (teamIds->isEmpty()) {
        if (callback) {
            callback({});
        }
        return;
    }

    auto collected = std::make_shared<QVector<ThreadSummary>>();
    queryUnreadTeamPage(teamIds, 0, QString(), collected, std::move(callback));
}

void ThreadFollowService::queryUnreadTeamPage(
    const std::shared_ptr<QStringList>& teamIds,
    int teamIndex,
    const QString& before,
    const std::shared_ptr<QVector<ThreadSummary>>& collected,
    ThreadListCallback callback)
{
    if (teamIndex >= teamIds->size()) {
        if (callback) {
            callback(*collected);
        }
        return;
    }

    const QString teamId = teamIds->at(teamIndex);
    QString path = QStringLiteral("users/") + backend.getLoginUser().id
        + QStringLiteral("/teams/") + teamId
        + QStringLiteral("/threads?unread=true&excludeDirect=true&per_page=")
        + QString::number(ThreadsPerPage);
    if (!before.isEmpty()) {
        path += QStringLiteral("&before=") + before;
    }

    NetworkRequest request(path);
    httpConnector.get(request, HttpResponseCallback(
        [this, teamIds, teamIndex, teamId, collected,
         callback = std::move(callback)](const QJsonDocument& doc) mutable {
            const QJsonArray threads = doc.object().value(QStringLiteral("threads")).toArray();
            QString lastThreadId;
            for (const QJsonValue& value : threads) {
                const QJsonObject object = value.toObject();
                const QJsonObject post = object.value(QStringLiteral("post")).toObject();

                ThreadSummary entry;
                entry.id = object.value(QStringLiteral("id")).toString();
                entry.channelId = post.value(QStringLiteral("channel_id")).toString();
                entry.teamId = teamId;
                entry.authorId = post.value(QStringLiteral("user_id")).toString();
                entry.message = post.value(QStringLiteral("message")).toString();
                entry.lastViewedAt = object.value(QStringLiteral("last_viewed_at"))
                    .toVariant().toULongLong();
                entry.lastReplyAt = object.value(QStringLiteral("last_reply_at"))
                    .toVariant().toULongLong();
                if (entry.lastReplyAt == 0) {
                    entry.lastReplyAt = post.value(QStringLiteral("create_at"))
                        .toVariant().toULongLong();
                }
                entry.unreadReplies = object.value(QStringLiteral("unread_replies")).toInt();
                entry.unreadMentions = object.value(QStringLiteral("unread_mentions")).toInt();
                entry.urgent = object.value(QStringLiteral("is_urgent")).toBool();

                if (!entry.id.isEmpty()) {
                    lastThreadId = entry.id;
                    collected->push_back(std::move(entry));
                }
            }

            if (threads.size() == ThreadsPerPage && !lastThreadId.isEmpty()) {
                queryUnreadTeamPage(teamIds, teamIndex, lastThreadId, collected,
                                    std::move(callback));
                return;
            }
            queryUnreadTeamPage(teamIds, teamIndex + 1, QString(), collected,
                                std::move(callback));
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
        // the UI optimistically; the next unread-thread query reconciles it.
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

void ThreadFollowService::markThreadRead(const QString& teamId,
                                         const QString& threadId,
                                         std::function<void(bool)> callback)
{
    if (teamId.isEmpty() || threadId.isEmpty() || backend.getLoginUser().id.isEmpty()) {
        if (callback) {
            callback(false);
        }
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    NetworkRequest request(threadPath(teamId, threadId)
                           + QStringLiteral("/read/") + QString::number(now));
    httpConnector.put(request, QByteArrayCreator(QJsonObject {}),
                      HttpResponseCallback([callback = std::move(callback)](
                                               QVariant status, const QJsonDocument&) mutable {
        if (callback) {
            callback(status.toInt() == QNetworkReply::NoError);
        }
    }));
}

} // namespace Mattermost
