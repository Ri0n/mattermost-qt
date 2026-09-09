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
#include <utility>

#include <QDateTime>
#include <QFont>
#include <QHeaderView>
#include <QIcon>
#include <QMouseEvent>
#include <QPointer>
#include <QSet>
#include <QSignalBlocker>

#include "backend/Backend.h"
#include "backend/SidebarService.h"
#include "backend/Storage.h"
#include "backend/UserProfileService.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "backend/types/BackendUser.h"
#include "channel-tree/ChannelIcons.h"
#include "navigation/AppNavigationService.h"

namespace Mattermost {

namespace {

constexpr int ThreadRefreshDelayMs = 300;
constexpr int ThreadSnippetLength = 120;

QString channelKey(const QString& channelId)
{
    return QStringLiteral("c:") + channelId;
}

QString threadKey(const QString& threadId)
{
    return QStringLiteral("t:") + threadId;
}

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

        // The selected row is allowed to become read without vanishing under
        // the cursor. Moving selection replaces this retention with the newly
        // selected row; the previous read row is then removed on refresh.
        retainSelection(current);
        QTimer::singleShot(0, this, &AttentionList::refresh);
        activateItem(current);
    });
}

void AttentionList::mousePressEvent(QMouseEvent* event)
{
    QTreeWidgetItem* pressedItem = nullptr;
    if (event && event->button() == Qt::LeftButton) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        pressedItem = itemAt(event->position().toPoint());
#else
        pressedItem = itemAt(event->pos());
#endif
    }

    // currentItemChanged handles the first click. A click on the already
    // selected row has no selection transition, so explicitly reactivate that
    // semantic destination after QTreeWidget processes the press.
    const bool repeatActivation = pressedItem && pressedItem == currentItem();
    QTreeWidget::mousePressEvent(event);
    if (repeatActivation && pressedItem == currentItem()) {
        activateItem(pressedItem);
    }
}

void AttentionList::activateItem(QTreeWidgetItem* current)
{
    if (refreshing || !current || !backend) {
        return;
    }

    const auto type = static_cast<EntryType>(current->data(0, EntryTypeRole).toInt());
    const QString channelId = current->data(0, ChannelIdRole).toString();
    if (type == ChannelEntry) {
        if (channelId.isEmpty()) {
            return;
        }

        // Once the first unread DM/GM post has been resolved, keep that exact
        // semantic target with the retained Attention row. Opening the channel
        // marks it read, so asking the server again on a repeat click would no
        // longer tell us which post the row originally represented.
        if (retainedThreadId.isEmpty()
            && retainedChannelId == channelId
            && !retainedPostId.isEmpty()) {
            AppNavigationService::instance(*backend).openPost(retainedPostId);
            return;
        }

        BackendChannel* channel = backend->getStorage().getChannelById(channelId);
        if (!channel) {
            emit channelSelected(channelId);
            return;
        }

        // Direct/group messages are implicitly followed. Like Following,
        // Attention opens the first unread post rather than the newest edge.
        QPointer<AttentionList> guard(this);
        backend->retrieveChannelUnreadPost(*channel,
            [guard, channelId](const QString& postId) {
                if (!guard || !guard->backend) {
                    return;
                }
                if (!postId.isEmpty()) {
                    if (guard->retainedChannelId == channelId
                        && guard->retainedThreadId.isEmpty()) {
                        guard->retainedPostId = postId;
                    }
                    AppNavigationService::instance(*guard->backend).openPost(postId);
                } else {
                    emit guard->channelSelected(channelId);
                }
            });
        return;
    }

    if (type == ThreadEntryType) {
        const QString threadId = current->data(0, ThreadIdRole).toString();
        const QString teamId = current->data(0, TeamIdRole).toString();
        if (!channelId.isEmpty() && !threadId.isEmpty()) {
            openThread(channelId, threadId, teamId);
        }
    }
}

void AttentionList::retainSelection(QTreeWidgetItem* item)
{
    retainedChannelId.clear();
    retainedThreadId.clear();
    retainedPostId.clear();
    hasRetainedThread = false;

    if (!item) {
        return;
    }

    retainedChannelId = item->data(0, ChannelIdRole).toString();
    const auto type = static_cast<EntryType>(item->data(0, EntryTypeRole).toInt());
    if (type != ThreadEntryType) {
        return;
    }

    retainedThreadId = item->data(0, ThreadIdRole).toString();
    if (retainedThreadId.isEmpty()) {
        return;
    }

    for (const ThreadEntry& thread : std::as_const(serverThreads)) {
        if (thread.id == retainedThreadId) {
            retainedThread = thread;
            hasRetainedThread = true;
            return;
        }
    }

    const auto syntheticIt = syntheticMentions.constFind(retainedThreadId);
    if (syntheticIt != syntheticMentions.cend()) {
        retainedThread = syntheticIt.value();
        hasRetainedThread = true;
    }
}

void AttentionList::releaseSelectionRetention()
{
    retainedChannelId.clear();
    retainedThreadId.clear();
    retainedPostId.clear();
    hasRetainedThread = false;

    // Attention is a queue rather than a navigation history. Leaving the tab
    // releases the sticky read row and also clears QTreeWidget's current item;
    // when the user returns, nothing is opened until they explicitly choose it.
    {
        const QSignalBlocker blocker(this);
        setCurrentItem(nullptr);
        clearSelection();
    }
    refresh();
}

void AttentionList::initialize(Backend& sourceBackend)
{
    backend = &sourceBackend;

    connect(backend, &Backend::onNewPost, this,
            [this](BackendChannel& channel, const BackendPost& post) {
        notePost(channel, post);
        refresh();

        // Followed-thread unread state is server-owned. Refresh it even while
        // this tab is hidden so the Attention badge/icon changes immediately.
        if (!post.root_id.isEmpty()) {
            scheduleThreadRefresh();
        }
    });
    connect(backend, &Backend::onChannelViewed, this,
            [this](const BackendChannel& channel) {
        clearSyntheticMentions(channel.id);
        refresh();
    });
    connect(backend, &Backend::onWebSocketConnect, this, [this] {
        scheduleThreadRefresh();
    });

    auto& sidebar = SidebarService::instance(*backend);
    connect(&sidebar, &SidebarService::channelActivityChanged, this,
            [this](const QString&) {
        // Navigation can acknowledge a DM/GM locally before Mattermost echoes
        // channel_viewed over the websocket. Rebuild now so the retained row
        // drops its bold Attention state immediately instead of waiting for a
        // later server thread/sidebar reconciliation.
        refresh();
    });
    connect(&sidebar, &SidebarService::channelActivityReset,
            this, &AttentionList::refresh);

    auto& followService = ThreadFollowService::instance(*backend);
    connect(&followService, &ThreadFollowService::followingChanged, this,
            [this](const QString&, const QString& threadId, bool following) {
        if (!following) {
            syntheticMentions.remove(threadId);
            pendingSince.remove(threadKey(threadId));
            for (auto it = serverThreads.begin(); it != serverThreads.end();) {
                if (it->id == threadId) {
                    it = serverThreads.erase(it);
                } else {
                    ++it;
                }
            }
            if (retainedThreadId == threadId) {
                retainedChannelId.clear();
                retainedThreadId.clear();
                retainedPostId.clear();
                hasRetainedThread = false;
                const QSignalBlocker blocker(this);
                setCurrentItem(nullptr);
                clearSelection();
            }
            refresh();
        }
        scheduleThreadRefresh();
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
            pendingSince.remove(threadKey(it.key()));
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
        QString key;
        uint64_t observedTime = 0;
        uint64_t sortTime = 0;
        bool attention = true;
    };

    const QString selectedChannel = !retainedChannelId.isEmpty()
        ? retainedChannelId
        : (currentItem() ? currentItem()->data(0, ChannelIdRole).toString() : QString());
    const QString selectedThread = !retainedChannelId.isEmpty()
        ? retainedThreadId
        : (currentItem() ? currentItem()->data(0, ThreadIdRole).toString() : QString());

    auto& sidebar = SidebarService::instance(*backend);
    QVector<DisplayEntry> entries;
    QSet<QString> displayedChannelIds;
    QSet<QString> displayedThreadIds;
    QSet<QString> presentKeys;

    auto stabilize = [this, &presentKeys](DisplayEntry& display) {
        presentKeys.insert(display.key);
        auto it = pendingSince.find(display.key);
        if (it == pendingSince.end()) {
            const uint64_t value = display.observedTime != 0
                ? display.observedTime
                : static_cast<uint64_t>(QDateTime::currentMSecsSinceEpoch());
            it = pendingSince.insert(display.key, value);
        }
        display.sortTime = it.value();
    };

    // Direct/group conversations are attention items as conversations, not as
    // thread rows. Muted conversations never require attention.
    for (auto it = backend->getStorage().channels.cbegin();
         it != backend->getStorage().channels.cend(); ++it) {
        BackendChannel* channel = it.value();
        if (!channel
            || (channel->type != BackendChannel::directChannel
                && channel->type != BackendChannel::groupChannel)
            || sidebar.isChannelMuted(*channel)
            || !sidebar.isChannelUnread(*channel)) {
            continue;
        }

        DisplayEntry display;
        display.type = ChannelEntry;
        display.channel = channel;
        display.key = channelKey(channel->id);
        display.observedTime = sidebar.channelActivityTime(*channel);
        stabilize(display);
        entries.push_back(std::move(display));
        displayedChannelIds.insert(channel->id);
    }

    QSet<QString> realThreadIds;
    for (const ThreadEntry& thread : std::as_const(serverThreads)) {
        if (thread.id.isEmpty() || thread.channelId.isEmpty()
            || (thread.unreadReplies <= 0 && thread.unreadMentions <= 0)) {
            continue;
        }
        BackendChannel* threadChannel = backend->getStorage().getChannelById(thread.channelId);
        if (!threadChannel || sidebar.isChannelMuted(*threadChannel)) {
            continue;
        }

        realThreadIds.insert(thread.id);
        displayedThreadIds.insert(thread.id);
        DisplayEntry display;
        display.type = ThreadEntryType;
        display.thread = thread;
        display.key = threadKey(thread.id);
        display.observedTime = thread.lastReplyAt;
        stabilize(display);
        entries.push_back(std::move(display));
    }

    // A root mention has no server Thread row yet. Mattermost sends recipient-
    // specific mention information on the websocket event, so model it as a
    // one-post synthetic thread until that channel is viewed. Muted channels
    // are intentionally excluded from Attention here as well.
    for (auto it = syntheticMentions.cbegin(); it != syntheticMentions.cend(); ++it) {
        if (realThreadIds.contains(it.key())) {
            continue;
        }
        BackendChannel* threadChannel = backend->getStorage().getChannelById(it->channelId);
        if (!threadChannel || sidebar.isChannelMuted(*threadChannel)) {
            continue;
        }
        displayedThreadIds.insert(it.key());
        DisplayEntry display;
        display.type = ThreadEntryType;
        display.thread = it.value();
        display.key = threadKey(it.key());
        display.observedTime = it->lastReplyAt;
        stabilize(display);
        entries.push_back(std::move(display));
    }

    // The badge reflects logical attention only. A selected row that became
    // read can remain visible below, but must stop contributing immediately.
    const uint32_t attentionCount = static_cast<uint32_t>(entries.size());
    if (lastAttentionCount != static_cast<int>(attentionCount)) {
        lastAttentionCount = static_cast<int>(attentionCount);
        emit attentionCountChanged(attentionCount);
    }

    // Preserve the selected item only when it merely became read. Muting is an
    // explicit action that disqualifies the row immediately, so muted channels
    // must never be resurrected by the sticky-selection rule.
    if (!retainedChannelId.isEmpty()) {
        if (retainedThreadId.isEmpty()) {
            if (!displayedChannelIds.contains(retainedChannelId)) {
                BackendChannel* channel = backend->getStorage().getChannelById(retainedChannelId);
                if (channel
                    && !sidebar.isChannelMuted(*channel)
                    && (channel->type == BackendChannel::directChannel
                        || channel->type == BackendChannel::groupChannel)) {
                    DisplayEntry display;
                    display.type = ChannelEntry;
                    display.channel = channel;
                    display.key = channelKey(channel->id);
                    display.observedTime = sidebar.channelActivityTime(*channel);
                    display.attention = false;
                    stabilize(display);
                    entries.push_back(std::move(display));
                }
            }
        } else if (hasRetainedThread && !displayedThreadIds.contains(retainedThreadId)) {
            BackendChannel* threadChannel = backend->getStorage().getChannelById(retainedThread.channelId);
            if (threadChannel && !sidebar.isChannelMuted(*threadChannel)) {
                DisplayEntry display;
                display.type = ThreadEntryType;
                display.thread = retainedThread;
                display.key = threadKey(retainedThread.id);
                display.observedTime = retainedThread.lastReplyAt;
                display.attention = false;
                stabilize(display);
                entries.push_back(std::move(display));
            }
        }
    }

    for (auto it = pendingSince.begin(); it != pendingSince.end();) {
        if (!presentKeys.contains(it.key())) {
            it = pendingSince.erase(it);
        } else {
            ++it;
        }
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
        font.setBold(display.attention);
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
                    QPointer<AttentionList> guard(this);
                    UserProfileService::instance(*backend).ensureUser(
                        channel.name, [guard](const BackendUser*) {
                            if (guard) {
                                guard->refresh();
                            }
                        });
                } else {
                    if (!user->avatar.isNull()) {
                        item->setIcon(0, QIcon(user->avatar));
                    } else {
                        UserProfileService::instance(*backend).ensureAvatar(*user);
                    }
                }
            } else if (channel.type == BackendChannel::groupChannel) {
                item->setIcon(0, ChannelIcons::groupConversation());
            }

            if (selectedThread.isEmpty() && selectedChannel == channel.id) {
                restoreItem = item;
            }
            continue;
        }

        const ThreadEntry& thread = display.thread;
        item->setText(0, threadLabel(thread));
        BackendChannel* threadChannel = backend->getStorage().getChannelById(thread.channelId);
        item->setIcon(0, threadChannel && threadChannel->type == BackendChannel::privateChannel
                             ? ChannelIcons::privateChannel()
                             : ChannelIcons::channel());
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
    } else {
        // QTreeWidget may otherwise keep/assign a current index while rows are
        // rebuilt. Attention must never implicitly open its first item.
        setCurrentItem(nullptr);
        clearSelection();
    }
    refreshing = false;
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

    threadRefreshInFlight = true;
    threadRefreshRequested = false;
    QPointer<AttentionList> guard(this);
    ThreadFollowService::instance(*backend).queryUnreadThreads(
        [guard](QVector<ThreadEntry> threads) {
            if (!guard) {
                return;
            }
            guard->serverThreads = std::move(threads);
            guard->threadRefreshInFlight = false;
            guard->refresh();
            if (guard->threadRefreshRequested) {
                guard->threadRefreshRequested = false;
                guard->scheduleThreadRefresh();
            }
        });
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

    // A root mention intentionally behaves like a one-message tracked thread,
    // but there is no actual thread until somebody replies. Keep that semantic
    // identity after the synthetic server-side queue entry has been consumed so
    // repeated clicks continue to jump/highlight the same root message.
    const bool queuedSynthetic = syntheticMentions.contains(threadId);
    const bool retainedSynthetic = hasRetainedThread
        && retainedThread.id == threadId
        && retainedThread.synthetic;
    if (queuedSynthetic || retainedSynthetic) {
        if (queuedSynthetic) {
            syntheticMentions.remove(threadId);
            pendingSince.remove(threadKey(threadId));
            refresh();
        }
        AppNavigationService::instance(*backend).openPost(threadId);
        return;
    }

    uint64_t lastViewedAt = 0;
    if (hasRetainedThread && retainedThread.id == threadId) {
        lastViewedAt = retainedThread.lastViewedAt;
    } else {
        for (const ThreadEntry& thread : std::as_const(serverThreads)) {
            if (thread.id == threadId) {
                lastViewedAt = thread.lastViewedAt;
                break;
            }
        }
    }

    // The unread ThreadResponse already gave us the exact server read boundary.
    // Fetch only a compact window beginning there, navigate to the first reply
    // after last_viewed_at, and acknowledge the thread only after that semantic
    // navigation has actually been dispatched. Attention is explicitly a jump
    // target, so an already-open thread must be repositioned/highlighted again.
    QPointer<AttentionList> guard(this);
    AppNavigationService::instance(*backend).openThreadAtLastViewed(
        channelId, threadId, lastViewedAt, QString(),
        [guard, teamId, threadId](bool opened) {
            if (!guard || !opened || guard->retainedThreadId != threadId) {
                return;
            }
            guard->markThreadRead(teamId, threadId);
        },
        false);
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
    refresh();
    ThreadFollowService::instance(*backend).markThreadRead(teamId, threadId);
}

} // namespace Mattermost
