/**
 * @file ChannelQuickList.cpp
 * @brief Flat channel index used by the Recent sidebar tab.
 */

#include "ChannelQuickList.h"

#include <algorithm>
#include <cstdint>

#include <QHeaderView>
#include <QIcon>
#include <QPointer>
#include <QVector>

#include "backend/Backend.h"
#include "backend/SidebarService.h"
#include "backend/Storage.h"
#include "backend/ThreadFollowService.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "backend/types/BackendTeam.h"
#include "backend/types/BackendUser.h"
#include "channel-tree/ChannelIcons.h"
#include "channel-tree/ChannelItemDelegate.h"
#include "channel-tree/SidebarItem.h"
#include "navigation/AppNavigationService.h"

namespace Mattermost {
namespace {

constexpr int RecentPostIdRole = Qt::UserRole + 100;

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

    connect(this, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* current, QTreeWidgetItem*) {
        if (refreshing || !current) {
            return;
        }

        const QString channelId = current->data(0, SidebarItem::IdRole).toString();
        const QString threadId = current->data(0, SidebarItem::ThreadIdRole).toString();
        const QString fallbackPostId = current->data(0, RecentPostIdRole).toString();

        if (backend && !threadId.isEmpty() && !channelId.isEmpty()) {
            BackendChannel* channel = backend->getStorage().getChannelById(channelId);
            const QString teamId = channel && channel->team ? channel->team->id : QString();
            if (teamId.isEmpty()) {
                if (!fallbackPostId.isEmpty()) {
                    AppNavigationService::instance(*backend).openPost(fallbackPostId);
                }
                return;
            }

            // A followed thread has its own read boundary. Query the canonical
            // ThreadResponse and navigate to the first reply after
            // last_viewed_at; if the thread is no longer followed, retain the
            // user's last local interaction as a deterministic fallback target.
            QPointer<ChannelQuickList> guard(this);
            ThreadFollowService::instance(*backend).queryThread(
                teamId, threadId,
                [guard, channelId, threadId, fallbackPostId](
                    const ThreadFollowService::ThreadState& state) {
                    if (!guard || !guard->backend) {
                        return;
                    }
                    auto& navigation = AppNavigationService::instance(*guard->backend);
                    if (state.available) {
                        navigation.openThreadAtLastViewed(channelId,
                                                          threadId,
                                                          state.lastViewedAt,
                                                          fallbackPostId);
                    } else if (!fallbackPostId.isEmpty()) {
                        navigation.openPost(fallbackPostId);
                    }
                });
            return;
        }

        if (!channelId.isEmpty()) {
            // ChannelTree/ChatArea owns read acknowledgement. It waits until
            // the selected channel's newest content has actually been rendered.
            emit channelSelected(channelId);
        }
    });

    connect(this, &QTreeWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        QTreeWidgetItem* item = itemAt(pos);
        if (!item) {
            return;
        }
        const QString channelId = item->data(0, SidebarItem::IdRole).toString();
        if (!channelId.isEmpty()) {
            emit channelContextMenuRequested(channelId, viewport()->mapToGlobal(pos));
        }
    });
}

void ChannelQuickList::initialize(Backend& sourceBackend, Mode)
{
    backend = &sourceBackend;

    // Mattermost's channel recency represents viewed/opened channels, not every
    // incoming post. Keep that behavior, but remember the user's latest thread
    // interaction. When that Recent row is opened, the followed-thread
    // last_viewed_at boundary is authoritative; the exact reply remains only a
    // fallback for unfollowed/already-read threads.
    connect(backend, &Backend::onNewPost, this,
            [this](BackendChannel& channel, const BackendPost& post) {
        if (!backend || post.user_id != backend->getLoginUser().id
            || post.root_id.isEmpty()) {
            return;
        }

        RecentThreadTarget target;
        target.rootPostId = post.root_id;
        target.fallbackPostId = post.id;
        target.interactionAt = post.create_at;

        const auto existing = recentThreadTargets.constFind(channel.id);
        if (existing == recentThreadTargets.cend()
            || existing->interactionAt <= target.interactionAt) {
            recentThreadTargets.insert(channel.id, std::move(target));
        }
        refresh();
    });

    refresh();
}

void ChannelQuickList::refresh()
{
    if (!backend) {
        return;
    }

    struct Candidate {
        BackendChannel* channel = nullptr;
        uint64_t sortTime = 0;
        bool unread = false;
        bool mentioned = false;
        QString recentPostId;
        QString recentRootId;
    };

    auto& sidebar = SidebarService::instance(*backend);
    QVector<Candidate> candidates;
    candidates.reserve(backend->getStorage().channels.size());

    for (auto it = backend->getStorage().channels.begin();
         it != backend->getStorage().channels.end(); ++it) {
        BackendChannel* channel = it.value();
        if (!channel) {
            continue;
        }

        const uint64_t channelRecentTime = sidebar.channelRecentTime(*channel);
        uint64_t sortTime = channelRecentTime;
        QString recentPostId;
        QString recentRootId;

        const auto target = recentThreadTargets.constFind(channel->id);
        if (target != recentThreadTargets.cend()
            && target->interactionAt >= channelRecentTime) {
            sortTime = std::max(sortTime, target->interactionAt);
            recentPostId = target->fallbackPostId;
            recentRootId = target->rootPostId;
        }

        if (sortTime == 0) {
            continue;
        }

        candidates.push_back(Candidate {
            channel,
            sortTime,
            sidebar.isChannelUnread(*channel),
            sidebar.hasUnreadMention(channel->id),
            recentPostId,
            recentRootId,
        });
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& lhs, const Candidate& rhs) {
        if (lhs.sortTime != rhs.sortTime) {
            return lhs.sortTime > rhs.sortTime;
        }
        return QString::localeAwareCompare(lhs.channel->display_name,
                                           rhs.channel->display_name) < 0;
    });

    // Keep the list compact like Mattermost's Quick Switcher, but unlike the
    // transient switcher this is a persistent navigation tab: the conversation
    // that is currently open must remain visible instead of disappearing after
    // the user selects it.
    if (candidates.size() > 20) {
        candidates.resize(20);
    }

    QString selectedChannelId;
    if (BackendChannel* currentChannel = backend->getCurrentChannel()) {
        selectedChannelId = currentChannel->id;
    } else if (currentItem()) {
        selectedChannelId = currentItem()->data(0, SidebarItem::IdRole).toString();
    }

    refreshing = true;
    clear();
    channelItems.clear();

    QTreeWidgetItem* itemToRestore = nullptr;
    for (const Candidate& candidate : candidates) {
        BackendChannel& channel = *candidate.channel;
        auto* item = new QTreeWidgetItem(this);

        QString displayName = channel.display_name;
        if (!candidate.recentPostId.isEmpty()) {
            displayName.prepend(QStringLiteral("↪ "));
            BackendPost* root = channel.postIdToPost.value(candidate.recentRootId, nullptr);
            if (root) {
                const QString summary = root->message.simplified();
                if (!summary.isEmpty()) {
                    displayName += QStringLiteral(" — ") + summary.left(60);
                }
            }
        }

        item->setText(0, displayName);
        item->setData(0, SidebarItem::KindRole, SidebarItem::Channel);
        item->setData(0, SidebarItem::IdRole, channel.id);
        item->setData(0, SidebarItem::ChannelIdRole, channel.id);
        item->setData(0, SidebarItem::ThreadIdRole, candidate.recentRootId);
        item->setData(0, RecentPostIdRole, candidate.recentPostId);
        item->setData(0, SidebarItem::ChannelTypeRole, channel.type);
        item->setData(0, SidebarItem::MutedRole, sidebar.isChannelMuted(channel));
        item->setData(0, SidebarItem::MentionedRole, candidate.mentioned);
        item->setData(0, SidebarItem::UnreadRole, candidate.unread);

        QString tooltip = channel.getTeamAndChannelName();
        if (!candidate.recentPostId.isEmpty()) {
            tooltip += tr("\nOpen recent thread interaction");
        }
        item->setToolTip(0, tooltip);

        if (channel.type == BackendChannel::directChannel) {
            BackendUser* user = backend->getStorage().getUserById(channel.name);
            if (user) {
                if (!user->avatar.isNull()) {
                    item->setIcon(0, QIcon(user->avatar));
                }
                item->setData(0, SidebarItem::PresenceRole, user->status);
                ensureDirectUserConnections(channel);
            }
        } else if (channel.type == BackendChannel::groupChannel) {
            item->setIcon(0, ChannelIcons::groupConversation());
        } else {
            item->setIcon(0, ChannelIcons::channel());
        }

        channelItems.insert(channel.id, item);
        if (channel.id == selectedChannelId) {
            itemToRestore = item;
        }
    }

    if (itemToRestore) {
        setCurrentItem(itemToRestore);
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
