/**
 * @file ChannelQuickList.cpp
 * @brief Followed conversation queue used by the Following sidebar tab.
 */

#include "ChannelQuickList.h"

#include <algorithm>
#include <cstdint>
#include <utility>

#include <QDateTime>
#include <QHeaderView>
#include <QIcon>
#include <QPointer>
#include <QShowEvent>
#include <QTabWidget>
#include <QVector>

#include "backend/Backend.h"
#include "backend/SidebarService.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "backend/types/BackendUser.h"
#include "channel-tree/ChannelIcons.h"
#include "channel-tree/ChannelItemDelegate.h"
#include "channel-tree/SidebarItem.h"
#include "navigation/AppNavigationService.h"

namespace Mattermost {
namespace {

constexpr int FollowingLastViewedRole = Qt::UserRole + 100;
constexpr int FollowingSyntheticRole = Qt::UserRole + 101;
constexpr int FollowingKeyRole = Qt::UserRole + 102;
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

ChannelQuickList::ChannelQuickList(QWidget* parent)
    : QTreeWidget(parent)
{
    setColumnCount(1);
    setHeaderHidden(true);
    setRootIsDecorated(false);
    setIndentation(0);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setUniformRowHeights(true);
    setContextMenuPolicy(Qt::CustomContextMenu);
    setItemDelegate(new ChannelItemDelegate(this));
    header()->setSectionResizeMode(0, QHeaderView::Stretch);

    threadRefreshTimer.setSingleShot(true);
    threadRefreshTimer.setInterval(ThreadRefreshDelayMs);
    connect(&threadRefreshTimer, &QTimer::timeout,
            this, &ChannelQuickList::refreshThreads);

    connect(this, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* current, QTreeWidgetItem*) {
        if (refreshing || !current || !backend) {
            return;
        }

        const QString channelId = current->data(0, SidebarItem::ChannelIdRole).toString();
        const QString threadId = current->data(0, SidebarItem::ThreadIdRole).toString();
        if (!threadId.isEmpty()) {
            ThreadSummary thread;
            thread.id = threadId;
            thread.channelId = channelId;
            thread.teamId = current->data(0, SidebarItem::TeamIdRole).toString();
            thread.lastViewedAt = current->data(0, FollowingLastViewedRole)
                .toULongLong();
            thread.synthetic = current->data(0, FollowingSyntheticRole).toBool();
            openThread(thread);
            return;
        }

        if (channelId.isEmpty()) {
            return;
        }

        BackendChannel* channel = backend->getStorage().getChannelById(channelId);
        if (!channel) {
            emit channelSelected(channelId);
            return;
        }

        // Direct/group conversations are implicitly followed. Open the actual
        // first unread post instead of jumping to the newest edge; ChatArea will
        // acknowledge the channel only when the newest content is really seen.
        QPointer<ChannelQuickList> guard(this);
        backend->retrieveChannelUnreadPost(*channel,
            [guard, channelId](const QString& postId) {
                if (!guard || !guard->backend) {
                    return;
                }
                if (!postId.isEmpty()) {
                    AppNavigationService::instance(*guard->backend).openPost(postId);
                } else {
                    emit guard->channelSelected(channelId);
                }
            });
    });

    connect(this, &QTreeWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        QTreeWidgetItem* item = itemAt(pos);
        if (!item) {
            return;
        }
        const QString channelId = item->data(0, SidebarItem::ChannelIdRole).toString();
        if (!channelId.isEmpty()) {
            emit channelContextMenuRequested(channelId, viewport()->mapToGlobal(pos));
        }
    });
}

void ChannelQuickList::initialize(Backend& sourceBackend, Mode)
{
    backend = &sourceBackend;

    if (auto* tabs = qobject_cast<QTabWidget*>(parentWidget())) {
        const int index = tabs->indexOf(this);
        if (index >= 0) {
            tabs->setTabText(index, tr("Following"));
            tabs->setTabToolTip(index,
                                tr("Unread direct messages and followed threads"));
        }
    }

    connect(backend, &Backend::onNewPost, this,
            [this](BackendChannel& channel, const BackendPost& post) {
        notePost(channel, post);

        const bool directConversation = channel.type == BackendChannel::directChannel
            || channel.type == BackendChannel::groupChannel;
        if (directConversation) {
            refresh();
            return;
        }

        // The server owns followed-thread membership/read state. Mark the
        // snapshot stale while hidden and reconcile it only when the tab is
        // visible. Merely switching tabs must not refetch an unchanged list.
        if (!post.root_id.isEmpty() || post.currentUserMentioned) {
            _threadSnapshotDirty = true;
            if (isVisible()) {
                scheduleThreadRefresh();
            }
        } else if (isVisible()) {
            refresh();
        }
    });

    connect(backend, &Backend::onChannelViewed, this,
            [this](const BackendChannel& channel) {
        clearSyntheticMentions(channel.id);
        refresh();
        _threadSnapshotDirty = true;
        if (isVisible()) {
            scheduleThreadRefresh();
        }
    });

    connect(backend, &Backend::onWebSocketConnect, this, [this] {
        _threadSnapshotDirty = true;
        if (isVisible()) {
            scheduleThreadRefresh();
        }
    });

    // Team/channel population is the first point at which the complete set of
    // team ids required by the CRT endpoint is authoritative. Always seed the
    // Following snapshot here, even if the tab is hidden.
    connect(backend, &Backend::onAllTeamChannelsPopulated, this, [this] {
        _threadSnapshotDirty = true;
        scheduleThreadRefresh();
    });

    auto& followService = ThreadFollowService::instance(*backend);
    connect(&followService, &ThreadFollowService::followingChanged, this,
            [this](const QString&, const QString& threadId, bool following) {
        if (!following) {
            syntheticMentions.remove(threadId);
            for (auto it = serverThreads.begin(); it != serverThreads.end();) {
                if (it->id == threadId) {
                    it = serverThreads.erase(it);
                } else {
                    ++it;
                }
            }
            pendingSince.remove(threadKey(threadId));
            refresh();
        }
        _threadSnapshotDirty = true;
        if (isVisible()) {
            scheduleThreadRefresh();
        }
    });

    // Catch root mentions that arrived before this view was constructed. A root
    // mention is the beginning of an implicitly followed thread even before the
    // server starts returning a concrete ThreadResponse for it.
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

void ChannelQuickList::showEvent(QShowEvent* event)
{
    QTreeWidget::showEvent(event);
    if (_threadSnapshotDirty) {
        scheduleThreadRefresh();
    }
}

void ChannelQuickList::notePost(BackendChannel& channel, const BackendPost& post)
{
    if (!post.currentUserMentioned || !post.root_id.isEmpty()) {
        return;
    }
    if (channel.type == BackendChannel::directChannel
        || channel.type == BackendChannel::groupChannel) {
        return;
    }

    ThreadSummary entry;
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

void ChannelQuickList::clearSyntheticMentions(const QString& channelId)
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

void ChannelQuickList::scheduleThreadRefresh()
{
    if (!threadRefreshTimer.isActive()) {
        threadRefreshTimer.start();
    }
}

void ChannelQuickList::refreshThreads()
{
    if (!backend) {
        return;
    }
    if (threadRefreshInFlight) {
        threadRefreshRequested = true;
        _threadSnapshotDirty = true;
        return;
    }

    threadRefreshInFlight = true;
    threadRefreshRequested = false;
    // Clear before starting the request. Any relevant event arriving while the
    // request is in flight marks the snapshot dirty again, so the callback can
    // distinguish a clean response from one that already needs reconciliation.
    _threadSnapshotDirty = false;
    QPointer<ChannelQuickList> guard(this);
    ThreadFollowService::instance(*backend).queryFollowingThreads(
        [guard](QVector<ThreadSummary> threads) {
            if (!guard) {
                return;
            }
            guard->serverThreads = std::move(threads);
            guard->threadRefreshInFlight = false;
            guard->refresh();
            if (guard->threadRefreshRequested) {
                guard->threadRefreshRequested = false;
                guard->scheduleThreadRefresh();
            } else if (guard->_threadSnapshotDirty && guard->isVisible()) {
                guard->scheduleThreadRefresh();
            }
        });
}

QString ChannelQuickList::threadLabel(const ThreadSummary& thread) const
{
    if (!backend) {
        return compactMessage(thread.message);
    }

    const BackendChannel* channel = backend->getStorage().getChannelById(thread.channelId);
    const QString channelName = channel ? channel->display_name : thread.channelId;
    const QString snippet = compactMessage(thread.message);
    const QString prefix = thread.synthetic || thread.unreadMentions > 0
        ? QStringLiteral("@ ") : QStringLiteral("\u21aa ");

    return snippet.isEmpty()
        ? prefix + channelName
        : prefix + channelName + QStringLiteral(" \u2014 ") + snippet;
}

void ChannelQuickList::openThread(const ThreadSummary& thread)
{
    if (!backend || thread.id.isEmpty() || thread.channelId.isEmpty()) {
        return;
    }

    if (thread.synthetic || syntheticMentions.contains(thread.id)) {
        syntheticMentions.remove(thread.id);
        pendingSince.remove(threadKey(thread.id));
        refresh();
        AppNavigationService::instance(*backend).openPost(thread.id);
        return;
    }

    QPointer<ChannelQuickList> guard(this);
    AppNavigationService::instance(*backend).openThreadAtLastViewed(
        thread.channelId, thread.id, thread.lastViewedAt, QString(),
        [guard, teamId = thread.teamId, threadId = thread.id](bool opened) {
            if (!guard || !guard->backend || !opened) {
                return;
            }

            bool wasUnread = false;
            for (ThreadSummary& entry : guard->serverThreads) {
                if (entry.id != threadId) {
                    continue;
                }
                wasUnread = entry.unreadReplies > 0 || entry.unreadMentions > 0;
                entry.unreadReplies = 0;
                entry.unreadMentions = 0;
                entry.lastViewedAt = qMax(entry.lastViewedAt, entry.lastReplyAt);
                break;
            }

            guard->pendingSince.remove(threadKey(threadId));
            guard->refresh();
            if (wasUnread) {
                ThreadFollowService::instance(*guard->backend).markThreadRead(teamId, threadId);
            }
        });
}

void ChannelQuickList::refresh()
{
    if (!backend) {
        return;
    }

    struct Candidate {
        QString key;
        BackendChannel* channel = nullptr;
        ThreadSummary thread;
        uint64_t observedTime = 0;
        uint64_t sortTime = 0;
        bool isThread = false;
        bool unread = false;
        bool mentioned = false;
    };

    auto& sidebar = SidebarService::instance(*backend);
    QVector<Candidate> candidates;
    QSet<QString> activeKeys;
    QSet<QString> realThreadIds;

    // DM/GM conversations are represented only while unread. Muted direct
    // conversations stay out of Following (bots are a common use of mute).
    for (auto it = backend->getStorage().channels.begin();
         it != backend->getStorage().channels.end(); ++it) {
        BackendChannel* channel = it.value();
        if (!channel
            || (channel->type != BackendChannel::directChannel
                && channel->type != BackendChannel::groupChannel)
            || sidebar.isChannelMuted(*channel)
            || !sidebar.isChannelUnread(*channel)) {
            continue;
        }

        Candidate candidate;
        candidate.key = channelKey(channel->id);
        candidate.channel = channel;
        candidate.observedTime = sidebar.channelActivityTime(*channel);
        candidate.unread = true;
        candidate.mentioned = sidebar.hasUnreadMention(channel->id);
        candidates.push_back(std::move(candidate));
    }

    // Unlike the old unread-only queue, Following mirrors Mattermost's default
    // Followed threads view: every followed thread remains present. Unread
    // threads are promoted above the read history and keep a stable position for
    // the lifetime of that unread cycle.
    for (const ThreadSummary& thread : std::as_const(serverThreads)) {
        if (thread.id.isEmpty() || thread.channelId.isEmpty()) {
            continue;
        }
        BackendChannel* channel = backend->getStorage().getChannelById(thread.channelId);
        if (!channel) {
            continue;
        }

        realThreadIds.insert(thread.id);
        Candidate candidate;
        candidate.key = threadKey(thread.id);
        candidate.thread = thread;
        candidate.observedTime = thread.lastReplyAt;
        candidate.isThread = true;
        candidate.unread = thread.unreadReplies > 0 || thread.unreadMentions > 0;
        candidate.mentioned = thread.unreadMentions > 0;
        candidates.push_back(std::move(candidate));
    }

    for (auto it = syntheticMentions.cbegin(); it != syntheticMentions.cend(); ++it) {
        if (realThreadIds.contains(it.key())) {
            continue;
        }
        BackendChannel* channel = backend->getStorage().getChannelById(it->channelId);
        if (!channel) {
            continue;
        }

        Candidate candidate;
        candidate.key = threadKey(it.key());
        candidate.thread = it.value();
        candidate.observedTime = it->lastReplyAt;
        candidate.isThread = true;
        candidate.unread = true;
        candidate.mentioned = true;
        candidates.push_back(std::move(candidate));
    }

    const uint64_t fallbackNow = static_cast<uint64_t>(QDateTime::currentMSecsSinceEpoch());
    for (Candidate& candidate : candidates) {
        activeKeys.insert(candidate.key);
        if (!candidate.unread) {
            pendingSince.remove(candidate.key);
            candidate.sortTime = candidate.observedTime;
            continue;
        }

        auto sortIt = pendingSince.find(candidate.key);
        if (sortIt == pendingSince.end()) {
            const uint64_t observed = candidate.observedTime != 0
                ? candidate.observedTime : fallbackNow;
            sortIt = pendingSince.insert(candidate.key, observed);
        }
        candidate.sortTime = sortIt.value();
    }

    for (auto it = pendingSince.begin(); it != pendingSince.end();) {
        if (!activeKeys.contains(it.key())) {
            it = pendingSince.erase(it);
        } else {
            ++it;
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& lhs, const Candidate& rhs) {
        if (lhs.unread != rhs.unread) {
            return lhs.unread;
        }
        if (lhs.sortTime != rhs.sortTime) {
            return lhs.sortTime > rhs.sortTime;
        }
        return lhs.key < rhs.key;
    });

    QString selectedKey = currentItem()
        ? currentItem()->data(0, FollowingKeyRole).toString() : QString();

    refreshing = true;
    clear();
    channelItems.clear();
    QTreeWidgetItem* itemToRestore = nullptr;

    for (const Candidate& candidate : candidates) {
        auto* item = new QTreeWidgetItem(this);
        item->setData(0, FollowingKeyRole, candidate.key);
        item->setData(0, SidebarItem::UnreadRole, candidate.unread);
        item->setData(0, SidebarItem::MentionedRole, candidate.mentioned);

        if (!candidate.isThread && candidate.channel) {
            BackendChannel& channel = *candidate.channel;
            item->setText(0, channel.display_name);
            item->setData(0, SidebarItem::KindRole, SidebarItem::Channel);
            item->setData(0, SidebarItem::IdRole, channel.id);
            item->setData(0, SidebarItem::ChannelIdRole, channel.id);
            item->setData(0, SidebarItem::ChannelTypeRole, channel.type);
            item->setData(0, SidebarItem::MutedRole, sidebar.isChannelMuted(channel));
            item->setToolTip(0, channel.getTeamAndChannelName());

            if (channel.type == BackendChannel::directChannel) {
                BackendUser* user = backend->getStorage().getUserById(channel.name);
                if (user) {
                    if (!user->avatar.isNull()) {
                        item->setIcon(0, QIcon(user->avatar));
                    }
                    item->setData(0, SidebarItem::PresenceRole, user->status);
                    ensureDirectUserConnections(channel);
                }
            } else {
                item->setIcon(0, ChannelIcons::groupConversation());
            }
            channelItems.insert(channel.id, item);
        } else {
            const ThreadSummary& thread = candidate.thread;
            BackendChannel* channel = backend->getStorage().getChannelById(thread.channelId);
            item->setText(0, threadLabel(thread));
            item->setData(0, SidebarItem::KindRole, SidebarItem::Thread);
            item->setData(0, SidebarItem::IdRole, thread.id);
            item->setData(0, SidebarItem::ChannelIdRole, thread.channelId);
            item->setData(0, SidebarItem::ThreadIdRole, thread.id);
            item->setData(0, SidebarItem::TeamIdRole, thread.teamId);
            item->setData(0, FollowingLastViewedRole,
                          QVariant::fromValue<qulonglong>(thread.lastViewedAt));
            item->setData(0, FollowingSyntheticRole, thread.synthetic);
            item->setData(0, SidebarItem::ChannelTypeRole,
                          channel ? channel->type : BackendChannel::publicChannel);
            item->setData(0, SidebarItem::MutedRole,
                          channel ? sidebar.isChannelMuted(*channel) : false);
            item->setToolTip(0, thread.message);

            if (channel && channel->type == BackendChannel::privateChannel) {
                item->setIcon(0, ChannelIcons::privateChannel());
            } else {
                item->setIcon(0, ChannelIcons::channel());
            }
        }

        if (candidate.key == selectedKey) {
            itemToRestore = item;
        }
    }

    if (itemToRestore) {
        setCurrentItem(itemToRestore);
    } else {
        setCurrentItem(nullptr);
        clearSelection();
    }
    refreshing = false;
}

void ChannelQuickList::ensureDirectUserConnections(BackendChannel& channel)
{
    if (!backend || channel.type != BackendChannel::directChannel) {
        return;
    }

    BackendUser* user = backend->getStorage().getUserById(channel.name);
    if (!user || connectedUsers.contains(user->id)) {
        return;
    }

    connectedUsers.insert(user->id);
    connect(user, &BackendUser::onStatusChanged, this, [this, user] {
        updateDirectUser(*user);
    });
    connect(user, &BackendUser::onAvatarChanged, this, [this, user] {
        updateDirectUser(*user);
    });
}

void ChannelQuickList::updateDirectUser(const BackendUser& user)
{
    if (!backend) {
        return;
    }

    BackendChannel* channel = backend->getStorage().getDirectChannelByUserId(user.id);
    if (!channel) {
        return;
    }

    QTreeWidgetItem* item = channelItems.value(channel->id, nullptr);
    if (!item) {
        return;
    }

    item->setData(0, SidebarItem::PresenceRole, user.status);
    if (!user.avatar.isNull()) {
        item->setIcon(0, QIcon(user.avatar));
    }
    viewport()->update();
}

} // namespace Mattermost
