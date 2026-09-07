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
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QPointer>

#include "Backend.h"
#include "NetworkRequest.h"
#include "QByteArrayCreator.h"
#include "SidebarService.h"
#include "Storage.h"
#include "types/BackendChannel.h"

namespace Mattermost {
namespace {

constexpr int ThreadsPerPage = 100;

Q_LOGGING_CATEGORY(lcFollowing, "mattermost.following")

void logFollowingState(Backend& backend,
                       const QVector<ThreadFollowService::ThreadSummary>& threads)
{
    auto& storage = backend.getStorage();
    auto& sidebar = SidebarService::instance(backend);

    int directChannels = 0;
    int groupChannels = 0;
    int unreadDirectChannels = 0;
    int unreadGroupChannels = 0;
    int mutedUnreadConversations = 0;

    for (auto it = storage.channels.cbegin(); it != storage.channels.cend(); ++it) {
        const BackendChannel* channel = it.value();
        if (!channel) {
            continue;
        }

        const bool direct = channel->type == BackendChannel::directChannel;
        const bool group = channel->type == BackendChannel::groupChannel;
        if (!direct && !group) {
            continue;
        }

        directChannels += direct ? 1 : 0;
        groupChannels += group ? 1 : 0;
        if (!sidebar.isChannelUnread(*channel)) {
            continue;
        }
        if (sidebar.isChannelMuted(*channel)) {
            ++mutedUnreadConversations;
            continue;
        }
        unreadDirectChannels += direct ? 1 : 0;
        unreadGroupChannels += group ? 1 : 0;
    }

    int emptyThreadId = 0;
    int emptyChannelId = 0;
    int noUnread = 0;
    int missingChannel = 0;
    int mutedThread = 0;
    int eligibleThreads = 0;
    qint64 unreadReplies = 0;
    qint64 unreadMentions = 0;

    for (const auto& thread : threads) {
        unreadReplies += thread.unreadReplies;
        unreadMentions += thread.unreadMentions;
        if (thread.id.isEmpty()) {
            ++emptyThreadId;
            continue;
        }
        if (thread.channelId.isEmpty()) {
            ++emptyChannelId;
            continue;
        }
        if (thread.unreadReplies <= 0 && thread.unreadMentions <= 0) {
            ++noUnread;
            continue;
        }

        const BackendChannel* channel = storage.getChannelById(thread.channelId);
        if (!channel) {
            ++missingChannel;
            continue;
        }
        if (sidebar.isChannelMuted(*channel)) {
            ++mutedThread;
            continue;
        }
        ++eligibleThreads;
    }

    qCDebug(lcFollowing).nospace()
        << "state channels=" << storage.channels.size()
        << " dm=" << directChannels
        << " gm=" << groupChannels
        << " unreadDm=" << unreadDirectChannels
        << " unreadGm=" << unreadGroupChannels
        << " mutedUnreadConversation=" << mutedUnreadConversations
        << " serverThreads=" << threads.size()
        << " eligibleThreads=" << eligibleThreads
        << " emptyThreadId=" << emptyThreadId
        << " emptyChannelId=" << emptyChannelId
        << " noUnread=" << noUnread
        << " missingChannel=" << missingChannel
        << " mutedThread=" << mutedThread
        << " unreadReplies=" << unreadReplies
        << " unreadMentions=" << unreadMentions
        << " expectedRows=" << (unreadDirectChannels + unreadGroupChannels + eligibleThreads);
}

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
        qCDebug(lcFollowing) << "query skipped: login user id is empty";
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

    qCDebug(lcFollowing).nospace()
        << "query start teams=" << teamIds->size()
        << " channels=" << backend.getStorage().channels.size();

    if (teamIds->isEmpty()) {
        qCDebug(lcFollowing) << "query finished immediately: no teams in Storage";
        logFollowingState(backend, {});
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
        qCDebug(lcFollowing).nospace()
            << "query complete collected=" << collected->size();
        logFollowingState(backend, *collected);
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

    const bool continuationPage = !before.isEmpty();
    qCDebug(lcFollowing).nospace()
        << "request team=" << (teamIndex + 1) << '/' << teamIds->size()
        << " page=" << (continuationPage ? "next" : "first");

    NetworkRequest request(path);
    httpConnector.get(request, HttpResponseCallback(
        [this, teamIds, teamIndex, teamId, continuationPage, collected,
         callback = std::move(callback)](const QJsonDocument& doc,
                                          const QNetworkReply& reply) mutable {
            const int httpStatus = reply.attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QJsonObject root = doc.object();
            const QJsonArray threads = root.value(QStringLiteral("threads")).toArray();

            qCDebug(lcFollowing).nospace()
                << "response team=" << (teamIndex + 1) << '/' << teamIds->size()
                << " page=" << (continuationPage ? "next" : "first")
                << " networkError=" << static_cast<int>(reply.error())
                << " http=" << httpStatus
                << " jsonObject=" << doc.isObject()
                << " pageThreads=" << threads.size()
                << " total=" << root.value(QStringLiteral("total")).toVariant().toLongLong()
                << " totalUnreadThreads="
                << root.value(QStringLiteral("total_unread_threads")).toVariant().toLongLong()
                << " totalUnreadMentions="
                << root.value(QStringLiteral("total_unread_mentions")).toVariant().toLongLong();

            int missingPost = 0;
            int missingChannelId = 0;
            int pageUnreadEntries = 0;
            QString lastThreadId;
            for (const QJsonValue& value : threads) {
                const QJsonObject object = value.toObject();
                if (!object.value(QStringLiteral("post")).isObject()) {
                    ++missingPost;
                }
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

                if (entry.channelId.isEmpty()) {
                    ++missingChannelId;
                }
                if (entry.unreadReplies > 0 || entry.unreadMentions > 0) {
                    ++pageUnreadEntries;
                }
                if (!entry.id.isEmpty()) {
                    lastThreadId = entry.id;
                    collected->push_back(std::move(entry));
                }
            }

            qCDebug(lcFollowing).nospace()
                << "parsed team=" << (teamIndex + 1) << '/' << teamIds->size()
                << " pageEntries=" << threads.size()
                << " unreadEntries=" << pageUnreadEntries
                << " missingPost=" << missingPost
                << " missingChannelId=" << missingChannelId
                << " collected=" << collected->size();

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
