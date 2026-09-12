/**
 * @file AttentionList.cpp
 * @brief Personal attention projection of the shared Following model.
 *
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of Mattermost-QT.
 */

#include "AttentionList.h"

#include <algorithm>
#include <utility>

#include <QFont>
#include <QHeaderView>
#include <QIcon>
#include <QMouseEvent>
#include <QPointer>
#include <QSignalBlocker>
#include <QTimer>
#include <QVector>

#include "backend/Backend.h"
#include "backend/SidebarService.h"
#include "backend/Storage.h"
#include "backend/UserProfileService.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendUser.h"
#include "channel-tree/ChannelIcons.h"
#include "channel-tree/FollowingNavigation.h"
#include "navigation/AppNavigationService.h"

namespace Mattermost {
namespace {

constexpr int ThreadSnippetLength = 120;
constexpr int SelectionSettleDelayMs = 180;

QString entryKey(const FollowingModel::Entry& entry)
{
    return entry.isThread()
        ? QStringLiteral("t:") + entry.threadId
        : QStringLiteral("c:") + entry.channelId;
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

    selectionRefreshTimer_ = new QTimer(this);
    selectionRefreshTimer_->setSingleShot(true);
    selectionRefreshTimer_->setInterval(SelectionSettleDelayMs);
    connect(selectionRefreshTimer_, &QTimer::timeout, this, [this] {
        selectionRefreshTimer_->stop();
        refresh();
    });

    connect(this, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* current, QTreeWidgetItem*) {
        if (refreshing_ || !current) {
            return;
        }

        // Change the navigation cursor immediately, but keep the old geometry
        // stable for a fraction of a second. Any synchronous/asynchronous model
        // updates caused by activation are coalesced by refresh() while the
        // settle timer is active.
        retainSelection(current);
        selectionRefreshTimer_->start();
        activateItem(current);
    });
}

void AttentionList::initialize(Backend& backend)
{
    backend_ = &backend;
    model_ = &FollowingModel::instance(backend);
    connect(model_, &FollowingModel::changed,
            this, &AttentionList::refresh);
    refresh();
    model_->ensureThreadsFresh();
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

    const bool repeatActivation = pressedItem && pressedItem == currentItem();
    QTreeWidget::mousePressEvent(event);
    if (repeatActivation && pressedItem == currentItem()) {
        activateItem(pressedItem);
    }
}

void AttentionList::retainSelection(QTreeWidgetItem* item)
{
    retainedEntry_.reset();
    if (!item || !model_) {
        return;
    }

    const QString channelId = item->data(0, SidebarItem::ChannelIdRole).toString();
    const QString threadId = item->data(0, SidebarItem::ThreadIdRole).toString();
    if (const FollowingModel::Entry* entry = model_->findEntry(channelId, threadId)) {
        retainedEntry_ = *entry;
    }
}

void AttentionList::releaseSelectionRetention()
{
    retainedEntry_.reset();
    {
        const QSignalBlocker blocker(this);
        setCurrentItem(nullptr);
        clearSelection();
    }

    // External navigation should have the same visual stability as selecting a
    // neighbouring Attention row: clear selection immediately, compact later.
    selectionRefreshTimer_->start();
}

void AttentionList::refreshThreads()
{
    if (model_) {
        model_->ensureThreadsFresh();
    }
}

void AttentionList::activateItem(QTreeWidgetItem* item)
{
    if (refreshing_ || !item || !backend_ || !model_) {
        return;
    }

    const QString channelId = item->data(0, SidebarItem::ChannelIdRole).toString();
    const QString threadId = item->data(0, SidebarItem::ThreadIdRole).toString();
    if (channelId.isEmpty()) {
        return;
    }

    const FollowingModel::Entry* entry = model_->findEntry(channelId, threadId);
    if (!entry) {
        // The row can outlive its attention entry, but its old cursor cannot.
        if (!retainedEntry_
            || retainedEntry_->channelId != channelId
            || retainedEntry_->threadId != threadId) {
            return;
        }

        if (retainedEntry_->isThread()) {
            if (retainedEntry_->synthetic) {
                model_->ensureThreadsFresh();
                AppNavigationService::instance(*backend_).openPost(threadId);
            } else {
                AppNavigationService::instance(*backend_).openThread(channelId, threadId);
            }
        } else {
            AppNavigationService::instance(*backend_).openChannel(channelId);
        }
        return;
    }

    if (entry->isThread()) {
        openThread(*entry);
        return;
    }

    if (entry->resumeState == FollowingModel::ResumeState::FirstUnread
        && !entry->firstUnreadPostId.isEmpty()) {
        AppNavigationService::instance(*backend_).openPost(entry->firstUnreadPostId);
        return;
    }
    if (entry->resumeState == FollowingModel::ResumeState::AtEnd) {
        AppNavigationService::instance(*backend_).openChannel(channelId);
        return;
    }

    BackendChannel* channel = backend_->getStorage().getChannelById(channelId);
    if (!channel) {
        emit channelSelected(channelId);
        return;
    }

    QPointer<AttentionList> guard(this);
    backend_->retrieveChannelUnreadPost(*channel,
        [guard, channelId](const QString& postId) {
            if (!guard || !guard->backend_ || !guard->model_ || !guard->retainedEntry_
                || guard->retainedEntry_->channelId != channelId
                || !guard->retainedEntry_->threadId.isEmpty()) {
                return;
            }

            const FollowingModel::Entry* current = guard->model_->findEntry(channelId);
            if (!current || current->resumeState == FollowingModel::ResumeState::AtEnd) {
                AppNavigationService::instance(*guard->backend_).openChannel(channelId);
                return;
            }
            if (current->resumeState == FollowingModel::ResumeState::FirstUnread
                && !current->firstUnreadPostId.isEmpty()) {
                AppNavigationService::instance(*guard->backend_).openPost(
                    current->firstUnreadPostId);
                return;
            }

            BackendChannel* currentChannel =
                guard->backend_->getStorage().getChannelById(channelId);
            if (!postId.isEmpty() && currentChannel
                && !isStaleConversationResumeTarget(*current, *currentChannel, postId)) {
                AppNavigationService::instance(*guard->backend_).openPost(postId);
            } else {
                // The server unread cursor can lag behind the local viewport
                // high-water mark until channel acknowledgement completes.
                // Never let that asynchronous fallback navigate backwards.
                AppNavigationService::instance(*guard->backend_).openChannel(channelId);
            }
        });
}

QString AttentionList::threadLabel(const FollowingModel::Entry& entry) const
{
    if (!backend_) {
        return compactMessage(entry.message);
    }

    const BackendChannel* channel = backend_->getStorage().getChannelById(entry.channelId);
    const QString channelName = channel ? channel->display_name : entry.channelId;
    const QString snippet = compactMessage(entry.message);
    const QString prefix = entry.synthetic || entry.unreadMentions > 0
        ? QStringLiteral("@ ") : QStringLiteral("\u21aa ");

    return snippet.isEmpty()
        ? prefix + channelName
        : prefix + channelName + QStringLiteral(" \u2014 ") + snippet;
}

void AttentionList::openThread(const FollowingModel::Entry& entry)
{
    if (!backend_ || !model_ || entry.threadId.isEmpty()) {
        return;
    }

    if (entry.synthetic) {
        // A click is navigation only. Keep the temporary mention until the
        // target is actually read; channel/thread viewport acknowledgement will
        // reconcile the shared Following model afterwards.
        model_->ensureThreadsFresh();
        AppNavigationService::instance(*backend_).openPost(entry.threadId);
        return;
    }

    if (entry.resumeState == FollowingModel::ResumeState::AtEnd) {
        AppNavigationService::instance(*backend_).openThread(entry.channelId, entry.threadId);
        return;
    }

    uint64_t resumeAfter = entry.lastViewedAt;
    if (entry.hasLocalProgress()) {
        resumeAfter = entry.readThroughCreateAt;
    }
    const QString fallbackPostId =
        entry.resumeState == FollowingModel::ResumeState::FirstUnread
        ? entry.firstUnreadPostId : QString();

    // Attention is a navigation request, not proof that the message was read.
    // Read acknowledgement comes from the thread viewport when its real newest
    // edge has been consumed, preserving the lower-edge rule for tall messages.
    AppNavigationService::instance(*backend_).openThreadAtLastViewed(
        entry.channelId, entry.threadId, resumeAfter, fallbackPostId, {}, false);
}

void AttentionList::refresh()
{
    if (!backend_ || !model_) {
        return;
    }

    // Selection changes intentionally leave the existing item geometry alone
    // for a short settle period. Model notifications arriving in that window
    // are not lost: the timer fires one refresh against the latest model state.
    if (selectionRefreshTimer_ && selectionRefreshTimer_->isActive()) {
        return;
    }

    struct DisplayEntry {
        FollowingModel::Entry entry;
        bool attention = true;
    };

    QVector<DisplayEntry> displayEntries;
    displayEntries.reserve(model_->entries().size() + 1);

    for (const FollowingModel::Entry& entry : model_->entries()) {
        if (!entry.requiresAttention() || entry.muted) {
            continue;
        }
        displayEntries.push_back(DisplayEntry { entry, true });
    }

    const uint32_t attentionCount = static_cast<uint32_t>(displayEntries.size());
    if (lastAttentionCount_ != static_cast<int>(attentionCount)) {
        lastAttentionCount_ = static_cast<int>(attentionCount);
        emit attentionCountChanged(attentionCount);
    }

    if (retainedEntry_) {
        const QString retainedKey = entryKey(*retainedEntry_);
        bool alreadyPresent = false;
        for (const DisplayEntry& display : std::as_const(displayEntries)) {
            if (entryKey(display.entry) == retainedKey) {
                alreadyPresent = true;
                break;
            }
        }

        if (!alreadyPresent) {
            const FollowingModel::Entry* current = model_->findEntry(
                retainedEntry_->channelId, retainedEntry_->threadId);
            if (current && !current->muted) {
                displayEntries.push_back(DisplayEntry { *current, false });
            } else if (!retainedEntry_->isThread()) {
                BackendChannel* channel = backend_->getStorage().getChannelById(
                    retainedEntry_->channelId);
                const bool muted = channel
                    ? SidebarService::instance(*backend_).isChannelMuted(*channel)
                    : true;
                if (channel && !muted) {
                    FollowingModel::Entry retained = *retainedEntry_;
                    retained.unread = false;
                    retained.mentioned = false;
                    retained.unreadReplies = 0;
                    retained.unreadMentions = 0;
                    retained.attentionSince = 0;
                    displayEntries.push_back(DisplayEntry { std::move(retained), false });
                }
            } else if (retainedEntry_->synthetic) {
                displayEntries.push_back(DisplayEntry { *retainedEntry_, false });
            }
        }
    }

    std::sort(displayEntries.begin(), displayEntries.end(),
              [](const DisplayEntry& lhs, const DisplayEntry& rhs) {
        const uint64_t lhsTime = lhs.entry.attentionSince != 0
            ? lhs.entry.attentionSince : lhs.entry.lastReplyAt;
        const uint64_t rhsTime = rhs.entry.attentionSince != 0
            ? rhs.entry.attentionSince : rhs.entry.lastReplyAt;
        if (lhsTime != rhsTime) {
            return lhsTime > rhsTime;
        }
        if (lhs.entry.isThread() != rhs.entry.isThread()) {
            return lhs.entry.isThread();
        }
        return entryKey(lhs.entry) < entryKey(rhs.entry);
    });

    const QString selectedKey = retainedEntry_ ? entryKey(*retainedEntry_) : QString();

    refreshing_ = true;
    clear();
    QTreeWidgetItem* restoreItem = nullptr;

    for (const DisplayEntry& display : std::as_const(displayEntries)) {
        const FollowingModel::Entry& entry = display.entry;
        auto* item = new QTreeWidgetItem(this);
        QFont font = item->font(0);
        font.setBold(display.attention);
        if (entry.urgent) {
            font.setUnderline(true);
        }
        item->setFont(0, font);
        item->setData(0, SidebarItem::ChannelIdRole, entry.channelId);
        item->setData(0, SidebarItem::ThreadIdRole, entry.threadId);
        item->setData(0, SidebarItem::TeamIdRole, entry.teamId);
        item->setData(0, SidebarItem::UnreadRole, display.attention);
        item->setData(0, SidebarItem::MentionedRole,
                      entry.mentioned || entry.unreadMentions > 0);
        item->setData(0, SidebarItem::MutedRole, entry.muted);

        if (!entry.isThread()) {
            BackendChannel* channel = backend_->getStorage().getChannelById(entry.channelId);
            if (!channel) {
                delete item;
                continue;
            }

            item->setText(0, channel->display_name);
            item->setData(0, SidebarItem::KindRole, SidebarItem::Channel);
            item->setData(0, SidebarItem::IdRole, channel->id);
            item->setData(0, SidebarItem::ChannelTypeRole, channel->type);
            item->setToolTip(0, channel->getTeamAndChannelName());

            if (channel->type == BackendChannel::directChannel) {
                BackendUser* user = backend_->getStorage().getUserById(channel->name);
                if (!user) {
                    QPointer<AttentionList> guard(this);
                    UserProfileService::instance(*backend_).ensureUser(
                        channel->name, [guard](const BackendUser*) {
                            if (guard) {
                                guard->refresh();
                            }
                        });
                } else if (!user->avatar.isNull()) {
                    item->setIcon(0, QIcon(user->avatar));
                } else {
                    UserProfileService::instance(*backend_).ensureAvatar(*user);
                }
            } else {
                item->setIcon(0, ChannelIcons::groupConversation());
            }
        } else {
            BackendChannel* channel = backend_->getStorage().getChannelById(entry.channelId);
            item->setText(0, threadLabel(entry));
            item->setData(0, SidebarItem::KindRole, SidebarItem::Thread);
            item->setData(0, SidebarItem::IdRole, entry.threadId);
            item->setData(0, SidebarItem::ChannelTypeRole,
                          channel ? channel->type : BackendChannel::publicChannel);
            item->setToolTip(0, entry.message);
            if (channel && channel->type == BackendChannel::privateChannel) {
                item->setIcon(0, ChannelIcons::privateChannel());
            } else {
                item->setIcon(0, ChannelIcons::channel());
            }
        }

        if (!selectedKey.isEmpty() && entryKey(entry) == selectedKey) {
            restoreItem = item;
        }
    }

    if (restoreItem) {
        setCurrentItem(restoreItem);
    } else {
        setCurrentItem(nullptr);
        clearSelection();
    }
    refreshing_ = false;
}

} // namespace Mattermost
