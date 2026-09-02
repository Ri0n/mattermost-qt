/**
 * @file AttentionList.cpp
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

#include "AttentionList.h"

#include <algorithm>

#include <QDateTime>
#include <QFont>
#include <QHeaderView>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "backend/Backend.h"
#include "backend/NetworkRequest.h"
#include "backend/QByteArrayCreator.h"
#include "backend/SidebarService.h"
#include "backend/Storage.h"
#include "backend/UserProfileService.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "backend/types/BackendTeam.h"
#include "backend/types/BackendUser.h"

namespace Mattermost {

namespace {

constexpr int ThreadsPerPage = 100;
constexpr int ThreadRefreshDelayMs = 300;
constexpr int ThreadSnippetLength = 120;

QString compactMessage(QString message)
{
    message = message.simplified();
    if (message.size() > ThreadSnippetLength) {
        message.truncate(ThreadSnippetLength - 1);
        message += QChar(0x2026);
    }
    return message;
}

} // namespace

AttentionList::AttentionList(QWidget* parent)
    : QTreeWidget(parent)
{
    setColumnCount(1);
    setHeaderHidden(true);
    setRootIsDecorated(false);
    setIndentation(0);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setUniformRowHeights(true);
    header()->setSectionResizeMode(0, QHeaderView::Stretch);

    threadRefreshTimer.setSingleShot(true);
    threadRefreshTimer.setInterval(ThreadRefreshDelayMs);
    connect(&threadRefreshTimer, &QTimer::timeout, this, &AttentionList::refreshThreads);

    connect(this, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* current, QTreeWidgetItem*) {
        if (refreshing || !current) {
            return;
        }

        const auto type = static_cast<EntryType>(current->data(0, EntryTypeRole).toInt());
        const QString channelId = current->data(0, ChannelIdRole).toString();
        if (type == ChannelEntry) {
            if (!channelId.isEmpty()) {
                emit channelSelected(channelId);
            }
            return;
        }

        if (type == ThreadEntryType) {
            const QString threadId = current->data(0, ThreadIdRole).toString();
            const QString teamId = current->data(0, TeamIdRole).toString();
            if (!channelId.isEmpty() && !threadId.isEmpty()) {
                openThread(channelId, threadId, teamId);
            }
        }
    });
}

void AttentionList::initialize(Backend& sourceBackend)
{
    backend = &sourceBackend;

    connect(&httpConnector, &HTTPConnector::onNetworkError,
            backend, &Backend::onNetworkError);
    connect(&httpConnector, &HTTPConnector::onHttpError,
            backend, &Backend::onHttpError);

    connect(backend, &Backend::onNewPost, this,
            [this](BackendChannel& channel, const BackendPost& post) {
        notePost(channel, post);
        refresh();
        if (!post.root_id.isEmpty() && isVisible()) {
            scheduleThreadRefresh();
        }
    });
    connect(backend, &Backend::onChannelViewed, this,
            [this](const BackendChannel& channel) {
        clearSyntheticMentions(channel.id);
        refresh();
    });
    connect(backend, &Backend::onWebSocketConnect, this, [this] {
        if (isVisible()) {
            scheduleThreadRefresh();
        }
    });

    // Catch recipient-specific mention flags on posts that may have arrived
    // between login and construction of the main window.
    for (auto it = backend->getStorage().channels.cbegin();
         it != backend->getStorage().channels.cend(); ++it) {
        BackendChannel* channel = it.value();
        if (!channel) {
            continue;
        }
        for (const BackendPost& post : channel->posts) {
            notePost(*channel, post);
        }
    }

    refresh();
}

void AttentionList::notePost(BackendChannel& channel, const BackendPost& post)
{
    if (!post.currentUserMentioned || !post.root_id.isEmpty()) {
        return;
    }
    if (channel.type == BackendChannel::directChannel
        || channel.type == BackendChannel::groupChannel) {
        // A DM/GM is already represented as a conversation entry. Do not add a
        // second synthetic thread row for an @mention inside the same chat.
        return;
    }

    ThreadEntry entry;
    entry.id = post.id;
    entry.channelId = channel.id;
    entry.teamId = channel.team ? channel.team->id : QString();
    entry.authorId = post.user_id;
    entry.message = post.message;
    entry.lastReplyAt = post.create_at;
    entry.unreadMentions = 1;
    entry.synthetic = true;
    syntheticMentions.insert(entry.id, std::move(entry));
}

void AttentionList::clearSyntheticMentions(const QString& channelId)
{
    for (auto it = syntheticMentions.begin(); it != syntheticMentions.end();) {
        if (it->channelId == channelId) {
            it = syntheticMentions.erase(it);
        } else {
            ++it;
        }
    }
}

QString AttentionList::threadLabel(const ThreadEntry& thread) const
{
    if (!backend) {
        return compactMessage(thread.message);
    }

    const BackendChannel* channel = backend->getStorage().getChannelById(thread.channelId);
    const QString channelName = channel
        ? channel->display_name
        : thread.channelId;
    const QString snippet = compactMessage(thread.message);

    QString prefix;
    if (thread.synthetic || thread.unreadMentions > 0) {
        prefix = QStringLiteral("@ ");
    } else {
        prefix = QStringLiteral("\u21aa ");
    }

    if (snippet.isEmpty()) {
        return prefix + channelName;
    }
    return prefix + channelName + QStringLiteral(" \u2014 ") + snippet;
}

void AttentionList::refresh()
{
    if (!backend) {
        return;
    }

    struct DisplayEntry {
        EntryType type = ChannelEntry;
        BackendChannel* channel = nullptr;
        ThreadEntry thread;
        uint64_t sortTime = 0;
    };

    const QString selectedType = currentItem()
        ? currentItem()->data(0, EntryTypeRole).toString()
        : QString();
    const QString selectedChannel = currentItem()
        ? currentItem()->data(0, ChannelIdRole).toString()
        : QString();
    const QString selectedThread = currentItem()
        ? currentItem()->data(0, ThreadIdRole).toString()
        : QString();

    auto& sidebar = SidebarService::instance(*backend);
    QVector<DisplayEntry> entries;

    // Direct/group conversations are attention items as conversations, not as
    // thread rows. Muted ordinary activity is already suppressed by the
    // channel-unread model; an explicit mention can still require attention.
    for (auto it = backend->getStorage().channels.cbegin();
         it != backend->getStorage().channels.cend(); ++it) {
        BackendChannel* channel = it.value();
        if (!channel
            || (channel->type != BackendChannel::directChannel
                && channel->type != BackendChannel::groupChannel)
            || !sidebar.isChannelUnread(*channel)) {
            continue;
        }

        DisplayEntry display;
        display.type = ChannelEntry;
        display.channel = channel;
        display.sortTime = sidebar.channelActivityTime(*channel);
        entries.push_back(std::move(display));
    }

    QSet<QString> realThreadIds;
    for (const ThreadEntry& thread : std::as_const(serverThreads)) {
        if (thread.id.isEmpty() || thread.channelId.isEmpty()
            || (thread.unreadReplies <= 0 && thread.unreadMentions <= 0)) {
            continue;
        }
        if (!backend->getStorage().getChannelById(thread.channelId)) {
            continue;
        }

        realThreadIds.insert(thread.id);
        DisplayEntry display;
        display.type = ThreadEntryType;
        display.thread = thread;
        display.sortTime = thread.lastReplyAt;
        entries.push_back(std::move(display));
    }

    // Mattermost only has a real Thread row after the conversation becomes a
    // thread. A root post that directly mentions this user is still personal
    // attention, so represent that recipient-specific websocket event as a
    // one-post synthetic thread until the channel is viewed.
    for (auto it = syntheticMentions.cbegin(); it != syntheticMentions.cend(); ++it) {
        if (realThreadIds.contains(it.key())) {
            continue;
        }
        if (!backend->getStorage().getChannelById(it->channelId)) {
            continue;
        }
        DisplayEntry display;
        display.type = ThreadEntryType;
        display.thread = it.value();
        display.sortTime = it->lastReplyAt;
        entries.push_back(std::move(display));
    }

    std::sort(entries.begin(), entries.end(), [](const DisplayEntry& lhs, const DisplayEntry& rhs) {
        if (lhs.sortTime != rhs.sortTime) {
            return lhs.sortTime > rhs.sortTime;
        }
        if (lhs.type != rhs.type) {
            return lhs.type == ThreadEntryType;
        }
        const QString lhsName = lhs.type == ChannelEntry && lhs.channel
            ? lhs.channel->display_name : lhs.thread.message;
        const QString rhsName = rhs.type == ChannelEntry && rhs.channel
            ? rhs.channel->display_name : rhs.thread.message;
        return QString::localeAwareCompare(lhsName, rhsName) < 0;
    });

    refreshing = true;
    clear();
    QTreeWidgetItem* restoreItem = nullptr;

    for (const DisplayEntry& display : std::as_const(entries)) {
        auto* item = new QTreeWidgetItem(this);
        QFont font = item->font(0);
        font.setBold(true);
        item->setFont(0, font);

        if (display.type == ChannelEntry && display.channel) {
            BackendChannel& channel = *display.channel;
            item->setText(0, channel.display_name);
            item->setData(0, EntryTypeRole, static_cast<int>(ChannelEntry));
            item->setData(0, ChannelIdRole, channel.id);
            item->setToolTip(0, channel.getTeamAndChannelName());

            if (channel.type == BackendChannel::directChannel) {
                BackendUser* user = backend->getStorage().getUserById(channel.name);
                if (!user) {
                    UserProfileService::instance(*backend).ensureUser(
                        channel.name, [this](const BackendUser*) { refresh(); });
                } else {
                    if (!user->avatar.isNull()) {
                        item->setIcon(0, QIcon(user->avatar));
                    } else {
                        UserProfileService::instance(*backend).ensureAvatar(*user);
                    }
                }
            }

            if (selectedThread.isEmpty() && selectedChannel == channel.id) {
                restoreItem = item;
            }
            continue;
        }

        const ThreadEntry& thread = display.thread;
        item->setText(0, threadLabel(thread));
        item->setData(0, EntryTypeRole, static_cast<int>(ThreadEntryType));
        item->setData(0, ChannelIdRole, thread.channelId);
        item->setData(0, ThreadIdRole, thread.id);
        item->setData(0, TeamIdRole, thread.teamId);
        item->setToolTip(0, thread.message);
        if (thread.urgent) {
            QFont urgentFont = item->font(0);
            urgentFont.setUnderline(true);
            item->setFont(0, urgentFont);
        }

        if (selectedChannel == thread.channelId && selectedThread == thread.id) {
            restoreItem = item;
        }
    }

    if (restoreItem) {
        setCurrentItem(restoreItem);
    }
    refreshing = false;
    Q_UNUSED(selectedType);
}

void AttentionList::scheduleThreadRefresh()
{
    if (!threadRefreshTimer.isActive()) {
        threadRefreshTimer.start();
    }
}

void AttentionList::refreshThreads()
{
    if (!backend) {
        return;
    }
    if (threadRefreshInFlight) {
        threadRefreshRequested = true;
        return;
    }

    QStringList teamIds;
    for (const auto& pair : backend->getStorage().teams) {
        if (!pair.first.isEmpty()) {
            teamIds.push_back(pair.first);
        }
    }

    if (teamIds.isEmpty() || backend->getLoginUser().id.isEmpty()) {
        return;
    }

    threadRefreshInFlight = true;
    threadRefreshRequested = false;
    const quint64 generation = ++threadRefreshGeneration;
    auto ids = std::make_shared<QStringList>(std::move(teamIds));
    auto collected = std::make_shared<QVector<ThreadEntry>>();
    fetchTeamPage(ids, 0, QString(), collected, generation);
}

void AttentionList::fetchTeamPage(const std::shared_ptr<QStringList>& teamIds,
                                  int teamIndex,
                                  const QString& before,
                                  const std::shared_ptr<QVector<ThreadEntry>>& collected,
                                  quint64 generation)
{
    if (!backend || generation != threadRefreshGeneration) {
        return;
    }
    if (teamIndex >= teamIds->size()) {
        finishThreadRefresh(collected, generation);
        return;
    }

    const QString teamId = teamIds->at(teamIndex);
    QString path = QStringLiteral("users/") + backend->getLoginUser().id
        + QStringLiteral("/teams/") + teamId
        + QStringLiteral("/threads?unread=true&excludeDirect=true&per_page=")
        + QString::number(ThreadsPerPage);
    if (!before.isEmpty()) {
        path += QStringLiteral("&before=") + before;
    }

    NetworkRequest request(path);
    httpConnector.get(request, HttpResponseCallback(
        [this, teamIds, teamIndex, teamId, collected, generation](const QJsonDocument& doc) {
            if (!backend || generation != threadRefreshGeneration) {
                return;
            }

            const QJsonArray threads = doc.object().value(QStringLiteral("threads")).toArray();
            QString lastThreadId;
            for (const QJsonValue& value : threads) {
                const QJsonObject object = value.toObject();
                const QJsonObject post = object.value(QStringLiteral("post")).toObject();

                ThreadEntry entry;
                entry.id = object.value(QStringLiteral("id")).toString();
                entry.channelId = post.value(QStringLiteral("channel_id")).toString();
                entry.teamId = teamId;
                entry.authorId = post.value(QStringLiteral("user_id")).toString();
                entry.message = post.value(QStringLiteral("message")).toString();
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
                fetchTeamPage(teamIds, teamIndex, lastThreadId, collected, generation);
                return;
            }
            fetchTeamPage(teamIds, teamIndex + 1, QString(), collected, generation);
        }));
}

void AttentionList::finishThreadRefresh(
    const std::shared_ptr<QVector<ThreadEntry>>& collected,
    quint64 generation)
{
    if (generation != threadRefreshGeneration) {
        return;
    }

    serverThreads = *collected;
    threadRefreshInFlight = false;
    refresh();

    if (threadRefreshRequested) {
        threadRefreshRequested = false;
        QTimer::singleShot(0, this, &AttentionList::refreshThreads);
    }
}

void AttentionList::openThread(const QString& channelId,
                               const QString& threadId,
                               const QString& teamId)
{
    if (!backend) {
        return;
    }

    BackendChannel* channel = backend->getStorage().getChannelById(channelId);
    if (!channel) {
        return;
    }

    NetworkRequest request(
        QStringLiteral("posts/") + threadId + QStringLiteral("/thread?per_page=100"));
    httpConnector.get(request, HttpResponseCallback(
        [this, channelId, threadId, teamId](const QJsonDocument& doc) {
            if (!backend) {
                return;
            }
            BackendChannel* currentChannel = backend->getStorage().getChannelById(channelId);
            if (!currentChannel) {
                return;
            }

            const QJsonObject root = doc.object();
            currentChannel->addPosts(root.value(QStringLiteral("order")).toArray(),
                                     root.value(QStringLiteral("posts")).toObject());
            emit threadSelected(channelId, threadId);
            markThreadRead(teamId, threadId);
        }));
}

void AttentionList::markThreadRead(const QString& teamId, const QString& threadId)
{
    if (!backend || teamId.isEmpty() || threadId.isEmpty()) {
        return;
    }

    for (auto it = serverThreads.begin(); it != serverThreads.end();) {
        if (it->id == threadId) {
            it = serverThreads.erase(it);
        } else {
            ++it;
        }
    }
    syntheticMentions.remove(threadId);
    refresh();

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    NetworkRequest request(
        QStringLiteral("users/") + backend->getLoginUser().id
        + QStringLiteral("/teams/") + teamId
        + QStringLiteral("/threads/") + threadId
        + QStringLiteral("/read/") + QString::number(now));
    httpConnector.put(request, QByteArrayCreator(QJsonObject {}),
                      HttpResponseCallback([](const QJsonDocument&) {}));
}

} // namespace Mattermost
