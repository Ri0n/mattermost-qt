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
 * the Free Software Foundation, either version 3 of the License, or
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

#include <QHash>
#include <QTimer>
#include <QTreeWidget>
#include <QVector>

#include "SidebarItem.h"
#include "backend/ThreadFollowService.h"

class QMouseEvent;

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
    void releaseSelectionRetention();

    QString channelIdAt(const QPoint& pos) const
    {
        QTreeWidgetItem* item = itemAt(pos);
        return item ? item->data(0, ChannelIdRole).toString() : QString();
    }

signals:
    void channelSelected(const QString& channelId);
    void threadSelected(const QString& channelId, const QString& rootPostId);
    void attentionCountChanged(uint32_t count);

protected:
    void mousePressEvent(QMouseEvent* event) override;

private:
    using ThreadEntry = ThreadFollowService::ThreadSummary;
    using EntryType = SidebarItem::Kind;

    // Transitional names keep the implementation readable without introducing
    // a second enum domain. Comparing/assigning them therefore remains strongly
    // typed as SidebarItem::Kind under -Wenum-compare.
    static constexpr EntryType ChannelEntry = SidebarItem::Channel;
    static constexpr EntryType ThreadEntryType = SidebarItem::Thread;

    enum : int {
        EntryTypeRole = SidebarItem::KindRole,
        ChannelIdRole = SidebarItem::ChannelIdRole,
        ThreadIdRole = SidebarItem::ThreadIdRole,
        TeamIdRole = SidebarItem::TeamIdRole,
    };

    void activateItem(QTreeWidgetItem* item);
    void retainSelection(QTreeWidgetItem* item);
    void notePost(BackendChannel& channel, const BackendPost& post);
    void clearSyntheticMentions(const QString& channelId);
    void scheduleThreadRefresh();
    void openThread(const QString& channelId, const QString& threadId, const QString& teamId);
    void markThreadRead(const QString& teamId, const QString& threadId);
    QString threadLabel(const ThreadEntry& thread) const;

    Backend* backend = nullptr;
    QVector<ThreadEntry> serverThreads;
    QHash<QString, ThreadEntry> syntheticMentions;
    QHash<QString, uint64_t> pendingSince;
    QTimer threadRefreshTimer;
    QString retainedChannelId;
    QString retainedThreadId;
    QString retainedPostId;
    ThreadEntry retainedThread;
    bool hasRetainedThread = false;
    bool refreshing = false;
    bool threadRefreshInFlight = false;
    bool threadRefreshRequested = false;
    int lastAttentionCount = -1;
};

} // namespace Mattermost
