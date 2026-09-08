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
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "ThreadFollowService.h"

#include <utility>

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

// Match the web client's normal Followed threads page size. The full followed
// history can contain thousands of entries, so it must not be exhausted during
// an ordinary refresh. Unread-only queries are small and are intentionally
// exhausted so Attention gets an authoritative snapshot.
constexpr int FollowingThreadsPerPage = 25;
constexpr int UnreadThreadsPerPage = 100;

Q_LOGGING_CATEGORY(lcFollowing, "mattermost.following")

void logFollowingState(Backend& backend,
                       const QVector<ThreadFollowService::ThreadSummary>& threads,
                       bool unreadOnly)
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
    int missingChannel = 0;
    int unreadThreads = 0;
    int readThreads = 0;
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
        if (!storage.getChannelById(thread.channelId)) {
            ++missingChannel;
            continue;
        }
        if (thread.unreadReplies > 0 || thread.unreadMentions > 0) {
            ++unreadThreads;
        } else {
            ++readThreads;
        }
    }

    qCDebug(lcFollowing).nospace()
        << "state mode=" << (unreadOnly ? "unread" : "all")
        << " channels=" << storage.channels.size()
        << " dm=" << directChannels
        << " gm=" << groupChannels
        << " unreadDm=" << unreadDirectChannels
        << " unreadGm=" << unreadGroupChannels
        << " mutedUnreadConversation=" << mutedUnreadConversations
        << " serverThreads=" << threads.size()
        << " unreadThreads=" << unreadThreads
        << " readThreads=" << readThreads
        << " emptyThreadId=" << emptyThreadId
        << " emptyChannelId=" << emptyChannelId
        << " missingChannel=" << missingChannel
        << " unreadReplies=" << unreadReplies
        << " unreadMentions=" << unreadMentions;
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
    , _backend(sourceBackend)
{
    // A GET for an unfollowed thread is expected to fail because the server's
    // GetThreadForUser path only exposes following memberships. Keep those
    // expected 4xx responses local instead of surfacing them as global errors.
}

QString ThreadFollowService::threadPath(const QString& teamId, const QString& threadId) const
{
    return QStringLiteral("users/") + _backend.getLoginUser().id
        + QStringLiteral("/teams/") + teamId
        + QStringLiteral("/threads/") + threadId;
}

void ThreadFollowService::queryThread(const QString& teamId,
                                      const QString& threadId,
                                      ThreadStateCallback callback)
{
    if (teamId.isEmpty() || threadId.isEmpty() || _backend.getLoginUser().id.isEmpty()) {
        if (callback) {
            callback(ThreadState {});
        }
        return;
    }

    // This endpoint returns the current user's ThreadResponse, not merely a
    // boolean following flag. Keep its read metadata so navigation can use the
    // same last_viewed_at boundary as the Mattermost web client.
    NetworkRequest request(threadPath(teamId, threadId));
    _httpConnector.get(request, HttpResponseCallback(
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

void ThreadFollowService::queryFollowingThreads(ThreadListCallback callback)
{
    // Mattermost keeps the normal Followed list and its Unreads filter as
    // separate snapshots. Some server versions do not populate unread counters
    // on the normal threadsOnly response, so overlay the authoritative unread
    // snapshot instead of trusting those fields. Unread threads outside the
    // first history page are appended so the view can still promote them.
    queryThreads(false,
                 [this, callback = std::move(callback)](QVector<ThreadSummary> followed) mutable {
        for (ThreadSummary& thread : followed) {
            thread.unreadReplies = 0;
            thread.unreadMentions = 0;
        }

        queryThreads(true,
                     [callback = std::move(callback), followed = std::move(followed)](
                         QVector<ThreadSummary> unread) mutable {
            QHash<QString, int> followedIndex;
            followedIndex.reserve(followed.size());
            for (int i = 0; i < followed.size(); ++i) {
                if (!followed.at(i).id.isEmpty()) {
                    followedIndex.insert(followed.at(i).id, i);
                }
            }

            int appendedUnread = 0;
            for (ThreadSummary& unreadThread : unread) {
                const auto it = followedIndex.constFind(unreadThread.id);
                if (it == followedIndex.cend()) {
                    followedIndex.insert(unreadThread.id, followed.size());
                    followed.push_back(std::move(unreadThread));
                    ++appendedUnread;
                    continue;
                }
                followed[*it] = std::move(unreadThread);
            }

            qCDebug(lcFollowing).nospace()
                << "merged mode=following history=" << (followed.size() - appendedUnread)
                << " unread=" << unread.size()
                << " appendedUnread=" << appendedUnread
                << " result=" << followed.size();

            if (callback) {
                callback(std::move(followed));
            }
        });
    });
}

void ThreadFollowService::queryUnreadThreads(ThreadListCallback callback)
{
    queryThreads(true, std::move(callback));
}

void ThreadFollowService::queryThreads(bool unreadOnly, ThreadListCallback callback)
{
    if (_backend.getLoginUser().id.isEmpty()) {
        qCDebug(lcFollowing) << "query skipped: login user id is empty";
        if (callback) {
            callback({});
        }
        return;
    }

    auto teamIds = std::make_shared<QStringList>();
    for (const auto& pair : _backend.getStorage().teams) {
        if (!pair.first.isEmpty()) {
            teamIds->push_back(pair.first);
        }
    }

    qCDebug(lcFollowing).nospace()
        << "query start mode=" << (unreadOnly ? "unread" : "all")
        << " teams=" << teamIds->size()
        << " channels=" << _backend.getStorage().channels.size();

    if (teamIds->isEmpty()) {
        qCDebug(lcFollowing) << "query finished immediately: no teams in Storage";
        logFollowingState(_backend, {}, unreadOnly);
        if (callback) {
            callback({});
        }
        return;
    }

    auto collected = std::make_shared<QVector<ThreadSummary>>();
    queryTeamPage(teamIds, 0, QString(), unreadOnly, collected, std::move(callback));
}

void ThreadFollowService::queryTeamPage(
    const std::shared_ptr<QStringList>& teamIds,
    int teamIndex,
    const QString& before,
    bool unreadOnly,
    const std::shared_ptr<QVector<ThreadSummary>>& collected,
    ThreadListCallback callback)
{
    if (teamIndex >= teamIds->size()) {
        qCDebug(lcFollowing).nospace()
            << "query complete mode=" << (unreadOnly ? "unread" : "all")
            << " collected=" << collected->size();
        logFollowingState(_backend, *collected, unreadOnly);
        if (callback) {
            callback(*collected);
        }
        return;
    }

    const QString teamId = teamIds->at(teamIndex);
    const int perPage = unreadOnly ? UnreadThreadsPerPage : FollowingThreadsPerPage;
    QString path = QStringLiteral("users/") + _backend.getLoginUser().id
        + QStringLiteral("/teams/") + teamId
        + QStringLiteral("/threads?threadsOnly=true&extended=true&excludeDirect=true&per_page=")
        + QString::number(perPage);
    if (unreadOnly) {
        path += QStringLiteral("&unread=true");
    }
    if (!before.isEmpty()) {
        path += QStringLiteral("&before=") + before;
    }

    const bool continuationPage = !before.isEmpty();
    qCDebug(lcFollowing).nospace()
        << "request mode=" << (unreadOnly ? "unread" : "all")
        << " team=" << (teamIndex + 1) << '/' << teamIds->size()
        << " page=" << (continuationPage ? "next" : "first")
        << " perPage=" << perPage;

    NetworkRequest request(path);
    _httpConnector.get(request, HttpResponseCallback(
        [this, teamIds, teamIndex, teamId, continuationPage, unreadOnly, perPage, collected,
         callback = std::move(callback)](const QJsonDocument& doc,
                                          const QNetworkReply& reply) mutable {
            const int httpStatus = reply.attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QJsonObject root = doc.object();
            const QJsonArray threads = root.value(QStringLiteral("threads")).toArray();

            qCDebug(lcFollowing).nospace()
                << "response mode=" << (unreadOnly ? "unread" : "all")
                << " team=" << (teamIndex + 1) << '/' << teamIds->size()
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
                << "parsed mode=" << (unreadOnly ? "unread" : "all")
                << " team=" << (teamIndex + 1) << '/' << teamIds->size()
                << " pageEntries=" << threads.size()
                << " unreadEntries=" << pageUnreadEntries
                << " missingPost=" << missingPost
                << " missingChannelId=" << missingChannelId
                << " collected=" << collected->size();

            // Following mirrors Mattermost's normal list and therefore loads
            // one page per team initially. Older followed history will be
            // requested by the view on demand instead of being exhausted here.
            if (!unreadOnly) {
                queryTeamPage(teamIds, teamIndex + 1, QString(), false, collected,
                              std::move(callback));
                return;
            }

            if (threads.size() == perPage && !lastThreadId.isEmpty()) {
                queryTeamPage(teamIds, teamIndex, lastThreadId, true, collected,
                              std::move(callback));
                return;
            }
            queryTeamPage(teamIds, teamIndex + 1, QString(), true, collected,
                          std::move(callback));
        }));
}

void ThreadFollowService::setFollowing(const QString& teamId,
                                       const QString& threadId,
                                       bool following,
                                       std::function<void(bool)> callback)
{
    if (teamId.isEmpty() || threadId.isEmpty() || _backend.getLoginUser().id.isEmpty()) {
        if (callback) {
            callback(false);
        }
        return;
    }

    NetworkRequest request(threadPath(teamId, threadId) + QStringLiteral("/following"));
    if (!following) {
        // HTTPConnector's DELETE API is intentionally fire-and-forget. Update
        // the UI optimistically; the next thread query reconciles it.
        _httpConnector.del(request);
        emit followingChanged(teamId, threadId, false);
        if (callback) {
            callback(true);
        }
        return;
    }

    _httpConnector.put(request, QByteArrayCreator(QJsonObject {}),
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
    if (teamId.isEmpty() || threadId.isEmpty() || _backend.getLoginUser().id.isEmpty()) {
        if (callback) {
            callback(false);
        }
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    NetworkRequest request(threadPath(teamId, threadId)
                           + QStringLiteral("/read/") + QString::number(now));
    _httpConnector.put(request, QByteArrayCreator(QJsonObject {}),
                       HttpResponseCallback([callback = std::move(callback)](
                                                QVariant status, const QJsonDocument&) mutable {
        if (callback) {
            callback(status.toInt() == QNetworkReply::NoError);
        }
    }));
}

} // namespace Mattermost
