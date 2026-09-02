/**
 * @file AttentionList.h
 * @brief Personal attention queue for direct conversations and followed threads.
 *
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Mattermost-QT is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Mattermost-QT. if not, see https://www.gnu.org/licenses/.
 */

#pragma once

#include <cstdint>
#include <memory>

#include <QHash>
#include <QTimer>
#include <QTreeWidget>
#include <QVector>

#include "backend/HTTPConnector.h"

class QHideEvent;

namespace Mattermost {

class Backend;
class BackendChannel;
class BackendPost;

class AttentionList : public QTreeWidget
{
    Q_OBJECT
public:
    explicit AttentionList(QWidget* parent = nullptr);

    void initialize(Backend& backend);
    void refresh();
    void refreshThreads();

signals:
    void channelSelected(const QString& channelId);
    void threadSelected(const QString& channelId, const QString& rootPostId);

protected:
    void hideEvent(QHideEvent* event) override;

private:
    struct ThreadEntry {
        QString id;
        QString channelId;
        QString teamId;
        QString authorId;
        QString message;
        uint64_t lastReplyAt = 0;
        int unreadReplies = 0;
        int unreadMentions = 0;
        bool urgent = false;
        bool synthetic = false;
    };

    enum EntryType {
        ChannelEntry = 1,
        ThreadEntryType = 2,
    };

    enum Role {
        EntryTypeRole = Qt::UserRole + 100,
        ChannelIdRole,
        ThreadIdRole,
        TeamIdRole,
    };

    void retainSelection(QTreeWidgetItem* item);
    void releaseRetainedSelection();
    void notePost(BackendChannel& channel, const BackendPost& post);
    void clearSyntheticMentions(const QString& channelId);
    void scheduleThreadRefresh();
    void fetchTeamPage(const std::shared_ptr<QStringList>& teamIds,
                       int teamIndex,
                       const QString& before,
                       const std::shared_ptr<QVector<ThreadEntry>>& collected,
                       quint64 generation);
    void finishThreadRefresh(const std::shared_ptr<QVector<ThreadEntry>>& collected,
                             quint64 generation);
    void openThread(const QString& channelId, const QString& threadId, const QString& teamId);
    void markThreadRead(const QString& teamId, const QString& threadId);
    QString threadLabel(const ThreadEntry& thread) const;

    Backend* backend = nullptr;
    HTTPConnector httpConnector;
    QVector<ThreadEntry> serverThreads;
    QHash<QString, ThreadEntry> syntheticMentions;
    QTimer threadRefreshTimer;
    QString retainedChannelId;
    QString retainedThreadId;
    ThreadEntry retainedThread;
    bool hasRetainedThread = false;
    bool refreshing = false;
    bool threadRefreshInFlight = false;
    bool threadRefreshRequested = false;
    quint64 threadRefreshGeneration = 0;
};

} // namespace Mattermost
