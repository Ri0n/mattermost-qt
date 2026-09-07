/**
 * @file ThreadFollowService.h
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

#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include "HTTPConnector.h"

namespace Mattermost {

class Backend;

class ThreadFollowService : public QObject
{
    Q_OBJECT
public:
    // Per-user ThreadResponse state. lastViewedAt is the canonical Mattermost
    // read boundary used by thread navigation.
    struct ThreadState {
        bool available = false;
        uint64_t lastViewedAt = 0;
        uint64_t lastReplyAt = 0;
        int unreadReplies = 0;
        int unreadMentions = 0;
    };

    /** Compact followed-thread row returned by Mattermost's CRT API. */
    struct ThreadSummary {
        QString id;
        QString channelId;
        QString teamId;
        QString authorId;
        QString message;
        uint64_t lastViewedAt = 0;
        uint64_t lastReplyAt = 0;
        int unreadReplies = 0;
        int unreadMentions = 0;
        bool urgent = false;
        bool synthetic = false;
    };

    using ThreadStateCallback = std::function<void(const ThreadState&)>;
    using ThreadListCallback = std::function<void(QVector<ThreadSummary>)>;

    static ThreadFollowService& instance(Backend& backend);

    void queryThread(const QString& teamId, const QString& threadId,
                     ThreadStateCallback callback);
    void queryFollowing(const QString& teamId, const QString& threadId,
                        std::function<void(bool)> callback);
    void queryFollowingThreads(ThreadListCallback callback);
    void queryUnreadThreads(ThreadListCallback callback);
    void setFollowing(const QString& teamId, const QString& threadId, bool following,
                      std::function<void(bool)> callback = {});
    void markThreadRead(const QString& teamId, const QString& threadId,
                        std::function<void(bool)> callback = {});

signals:
    void followingChanged(const QString& teamId, const QString& threadId, bool following);

private:
    explicit ThreadFollowService(Backend& backend);
    QString threadPath(const QString& teamId, const QString& threadId) const;
    void queryThreads(bool unreadOnly, ThreadListCallback callback);
    void queryTeamPage(const std::shared_ptr<QStringList>& teamIds,
                       int teamIndex,
                       const QString& before,
                       bool unreadOnly,
                       const std::shared_ptr<QVector<ThreadSummary>>& collected,
                       ThreadListCallback callback);

    Backend& _backend;
    HTTPConnector _httpConnector;
};

} // namespace Mattermost
