/**
 * @file ChannelQuickList.cpp
 * @brief Flat channel index used by the Recent and Unreads sidebar tabs.
 */

#include "ChannelQuickList.h"

#include <algorithm>

#include <QHeaderView>
#include <QIcon>
#include <QVector>

#include "backend/Backend.h"
#include "backend/SidebarService.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendUser.h"
#include "channel-tree/ChannelItemDelegate.h"
#include "channel-tree/ChannelTree.h"

namespace Mattermost {

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
    setItemDelegate(new ChannelItemDelegate(this));
    header()->setSectionResizeMode(0, QHeaderView::Stretch);

    connect(this, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* current, QTreeWidgetItem*) {
        if (refreshing || !current) {
            return;
        }
        const QString channelId = current->data(0, ChannelTree::ItemIdRole).toString();
        if (!channelId.isEmpty()) {
            emit channelSelected(channelId);
        }
    });
}

void ChannelQuickList::initialize(Backend& sourceBackend, Mode listMode)
{
    backend = &sourceBackend;
    mode = listMode;
    refresh();
}

void ChannelQuickList::refresh()
{
    if (!backend) {
        return;
    }

    struct Candidate {
        BackendChannel* channel = nullptr;
        quint64 activity = 0;
        bool unread = false;
    };

    auto& sidebar = SidebarService::instance(*backend);
    QVector<Candidate> candidates;
    candidates.reserve(backend->getStorage().channels.size());

    for (auto it = backend->getStorage().channels.begin();
         it != backend->getStorage().channels.end(); ++it) {
        BackendChannel* channel = it.value();
        if (!channel || !sidebar.isChannelTracked(channel->id)) {
            continue;
        }

        const bool unread = sidebar.isChannelUnread(*channel);
        if (mode == Unreads && !unread) {
            continue;
        }

        candidates.push_back(Candidate {
            channel,
            sidebar.channelActivityTime(*channel),
            unread,
        });
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& lhs, const Candidate& rhs) {
        if (lhs.activity != rhs.activity) {
            return lhs.activity > rhs.activity;
        }
        return QString::localeAwareCompare(lhs.channel->display_name,
                                           rhs.channel->display_name) < 0;
    });

    const QString selectedChannelId = currentItem()
        ? currentItem()->data(0, ChannelTree::ItemIdRole).toString()
        : QString();

    refreshing = true;
    clear();
    channelItems.clear();

    QTreeWidgetItem* itemToRestore = nullptr;
    for (const Candidate& candidate : candidates) {
        BackendChannel& channel = *candidate.channel;
        auto* item = new QTreeWidgetItem(this);
        item->setText(0, channel.display_name);
        item->setData(0, ChannelTree::ItemKindRole, ChannelTree::ChannelItemKind);
        item->setData(0, ChannelTree::ItemIdRole, channel.id);
        item->setData(0, ChannelTree::ItemMutedRole, sidebar.isChannelMuted(channel));
        item->setData(0, ChannelTree::ItemMentionedRole, sidebar.hasUnreadMention(channel.id));
        item->setData(0, ChannelTree::ItemUnreadRole, candidate.unread);
        item->setToolTip(0, channel.getTeamAndChannelName());

        if (channel.type == BackendChannel::directChannel) {
            BackendUser* user = backend->getStorage().getUserById(channel.name);
            if (user) {
                if (!user->avatar.isNull()) {
                    item->setIcon(0, QIcon(user->avatar));
                }
                item->setData(0, ChannelTree::ItemStatusRole, user->status);
                ensureDirectUserConnections(channel);
            }
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

    item->setData(0, ChannelTree::ItemStatusRole, user.status);
    if (!user.avatar.isNull()) {
        item->setIcon(0, QIcon(user.avatar));
    }
    viewport()->update();
}

} // namespace Mattermost
