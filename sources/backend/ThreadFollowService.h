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
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <cstdint>
#include <functional>

#include <QObject>
#include <QString>

#include "HTTPConnector.h"

namespace Mattermost {

class Backend;

class ThreadFollowService : public QObject
{
    Q_OBJECT
public:
    struct ThreadState {
        bool available = false;
        uint64_t lastViewedAt = 0;
        uint64_t lastReplyAt = 0;
        int unreadReplies = 0;
        int unreadMentions = 0;
    };

    using ThreadStateCallback = std::function<void(const ThreadState&)>;

    static ThreadFollowService& instance(Backend& backend);

    void queryThread(const QString& teamId, const QString& threadId,
                     ThreadStateCallback callback);
    void queryFollowing(const QString& teamId, const QString& threadId,
                        std::function<void(bool)> callback);
    void setFollowing(const QString& teamId, const QString& threadId, bool following,
                      std::function<void(bool)> callback = {});

signals:
    void followingChanged(const QString& teamId, const QString& threadId, bool following);

private:
    explicit ThreadFollowService(Backend& backend);
    QString threadPath(const QString& teamId, const QString& threadId) const;

    Backend& backend;
    HTTPConnector httpConnector;
};

} // namespace Mattermost
