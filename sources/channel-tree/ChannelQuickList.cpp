/**
 * @file ChannelQuickList.cpp
 * @brief Flat channel index used by the Recent sidebar tab.
 */

#include "ChannelQuickList.h"

#include <algorithm>
#include <cstdint>

#include <QHeaderView>
#include <QIcon>
#include <QVector>

#include "backend/Backend.h"
#include "backend/SidebarService.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendUser.h"
#include "channel-tree/ChannelIcons.h"
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
    setContextMenuPolicy(Qt::CustomContextMenu);
    setItemDelegate(new ChannelItemDelegate(this));
    header()->setSectionResizeMode(0, QHeaderView::Stretch);

    connect(this, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* current, QTreeWidgetItem*) {
        if (refreshing || !current) {
            return;
        }
        const QString channelId = current->data(0, ChannelTree::ItemIdRole).toString();
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
        const QString channelId = item->data(0, ChannelTree::ItemIdRole).toString();
        if (!channelId.isEmpty()) {
            emit channelContextMenuRequested(channelId, viewport()->mapToGlobal(pos));
        }
    });
}

void ChannelQuickList::initialize(Backend& sourceBackend, Mode)
{
    backend = &sourceBackend;
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

        const uint64_t sortTime = sidebar.channelRecentTime(*channel);
        if (sortTime == 0) {
            continue;
        }

        candidates.push_back(Candidate {
            channel,
            sortTime,
            sidebar.isChannelUnread(*channel),
            sidebar.hasUnreadMention(channel->id),
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
        selectedChannelId = currentItem()->data(0, ChannelTree::ItemIdRole).toString();
    }

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
        item->setData(0, ChannelTree::ItemMentionedRole, candidate.mentioned);
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

    item->setData(0, ChannelTree::ItemStatusRole, user.status);
    if (!user.avatar.isNull()) {
        item->setIcon(0, QIcon(user.avatar));
    }
    viewport()->update();
}

} // namespace Mattermost
