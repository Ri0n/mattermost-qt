/**
 * @file ChannelTree.cpp
 * @brief
 * @author Lyubomir Filipov
 * @date Dec 18, 2021
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

#include "ChannelTree.h"

#include <QDropEvent>
#include <QHeaderView>
#include <QPointer>
#include <QStackedWidget>

#include "backend/Backend.h"
#include "backend/SidebarService.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendTeam.h"
#include "chat-area/ChatArea.h"
#include "channel-tree/ChannelIcons.h"
#include "channel-tree/ChannelItem.h"
#include "channel-tree/ChannelItemDelegate.h"
#include "channel-tree/channel-item/DirectChannelItem.h"
#include "channel-tree/team-item/GroupTeamItem.h"
#include "log.h"

namespace Mattermost {

namespace {

QString categoryDisplayName(const SidebarCategory& category)
{
    if (!category.displayName.isEmpty()) {
        return category.displayName;
    }
    if (category.type == QStringLiteral("favorites")) {
        return QStringLiteral("Favorites");
    }
    if (category.type == QStringLiteral("channels")) {
        return QStringLiteral("Channels");
    }
    if (category.type == QStringLiteral("direct_messages")) {
        return QStringLiteral("Direct Messages");
    }
    return QStringLiteral("Category");
}

QPoint dropEventPosition(const QDropEvent* event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->position().toPoint();
#else
    return event->pos();
#endif
}

} // namespace

ChannelTree::ChannelTree (QWidget* parent)
:QTreeWidget (parent)
,chatAreaStackedWidget(nullptr)
,backendForSidebar(nullptr)
,renderingSidebar(false)
{
	connect (this, &QTreeWidget::customContextMenuRequested, this, &ChannelTree::showContextMenu);

	connect (this, &QTreeWidget::currentItemChanged, this,
             [this] (QTreeWidgetItem* item, QTreeWidgetItem*) {
        activateChannelItem(item);
	});

    connect(this, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* item) {
        if (!renderingSidebar) {
            setCategoryCollapsed(item, false);
        }
    });
    connect(this, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem* item) {
        if (!renderingSidebar) {
            setCategoryCollapsed(item, true);
        }
    });

    setItemDelegate(new ChannelItemDelegate(this));
    setMouseTracking(true);
    setDragEnabled(true);
    setAcceptDrops(true);
    viewport()->setAcceptDrops(true);
    setDropIndicatorShown(true);
    setDragDropMode(QAbstractItemView::InternalMove);
    setDefaultDropAction(Qt::MoveAction);
}

ChannelTree::~ChannelTree () = default;

bool ChannelTree::isChannelActive (const BackendChannel& channel)
{
	return getCurrentPage () && &getCurrentPage()->getChannel() == &channel;
}

void ChannelTree::addTeam (Backend& backend, BackendTeam& team)
{
    backendForSidebar = &backend;

	TeamItem* teamList = new GroupTeamItem (*this, backend, team.display_name, team.id);
    teamList->setData(0, ItemKindRole, TeamItemKind);
    teamList->setData(0, ItemIdRole, team.id);
    teamList->setData(0, ItemTeamIdRole, team.id);
    teamList->setFlags(teamList->flags() & ~(Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled));

	addTopLevelItem (teamList);
    teamToItemMap.insert(team.id, teamList);
	header()->setSectionResizeMode(0, QHeaderView::Stretch);
	header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);

	connect (&team, &BackendTeam::onNewChannel, this, [this, &backend, &team] (BackendChannel&) {
        refreshTeamSidebar(backend, team);
	});

	connect (&team, &BackendTeam::onLeave, this, [this, &team, teamList] {
        clearTeamSidebar(*teamList);
        teamToItemMap.remove(team.id);

		int index = indexOfTopLevelItem (teamList);
		if (index == -1) {
			return;
		}

		QTreeWidgetItem* item = takeTopLevelItem (index);
		delete item;
	});

    // Populate only channels at startup. Team members are loaded on demand by
    // the dialogs that actually need them; downloading every member here is a
    // large and unrelated startup request burst on big Mattermost instances.
	backend.retrieveOwnChannelMembershipsForTeam (team, [] (BackendChannel&) {});
}

void ChannelTree::populateSidebars(Backend& backend)
{
    backendForSidebar = &backend;
    auto& sidebar = SidebarService::instance(backend);
    connect(&sidebar, &SidebarService::channelMutedChanged,
            this, &ChannelTree::setChannelMutedVisual, Qt::UniqueConnection);
    connect(&sidebar, &SidebarService::channelMentionedChanged,
            this, &ChannelTree::setChannelMentionedVisual, Qt::UniqueConnection);

    for (auto it = teamToItemMap.cbegin(); it != teamToItemMap.cend(); ++it) {
        if (BackendTeam* team = backend.getStorage().getTeamById(it.key())) {
            refreshTeamSidebar(backend, *team);
        }
    }
}

void ChannelTree::refreshTeamSidebar(Backend& backend, BackendTeam& team)
{
    const QString teamId = team.id;
    SidebarService::instance(backend).retrieveCategories(team,
        [this, &backend, teamId](const SidebarTeamState& state) {
            BackendTeam* currentTeam = backend.getStorage().getTeamById(teamId);
            TeamItem* teamItem = teamToItemMap.value(teamId, nullptr);
            if (!currentTeam || !teamItem) {
                return;
            }
            renderTeamSidebar(backend, *teamItem, state);
        });
}

void ChannelTree::renderTeamSidebar(Backend& backend, TeamItem& teamItem,
                                    const SidebarTeamState& state)
{
    renderingSidebar = true;
    clearTeamSidebar(teamItem);

    auto& sidebar = SidebarService::instance(backend);
    BackendChannel* personalChannel = backend.getStorage().getDirectChannelByUserId(
        backend.getLoginUser().id);
    for (const auto& categoryId : state.order) {
        const SidebarCategory* category = state.category(categoryId);
        if (!category) {
            continue;
        }

        QTreeWidgetItem* categoryItem = createCategoryItem(
            teamItem, category->id, categoryDisplayName(*category), category->collapsed);

        const bool favorites = category->type == QStringLiteral("favorites");
        if (favorites) {
            createPersonalItem(backend, teamItem, *categoryItem);
        }

        const QStringList visibleIds = sidebar.visibleChannelIds(*category);
        for (const auto& channelId : visibleIds) {
            BackendChannel* channel = backend.getStorage().getChannelById(channelId);
            if (favorites && personalChannel && channel == personalChannel) {
                // Personal is the canonical user-facing row inside Favorites.
                // Do not render the same self-DM twice in that category.
                continue;
            }
            if (!channel) {
                LOG_DEBUG("Sidebar channel not found in storage: " << channelId);
                continue;
            }
            createChannelItem(backend, teamItem, *categoryItem, *channel);
        }
    }

    teamItem.setExpanded(true);
    renderingSidebar = false;
}

void ChannelTree::clearTeamSidebar(TeamItem& teamItem)
{
    while (teamItem.childCount() > 0) {
        QTreeWidgetItem* category = teamItem.takeChild(0);
        while (category->childCount() > 0) {
            QTreeWidgetItem* channelItem = category->takeChild(0);
            const QString channelId = channelItem->data(0, ItemIdRole).toString();
            removeChannelToItem(channelId, channelItem);

            ChatArea* chatArea = channelItem->data(0, Qt::UserRole).value<ChatArea*>();
            if (chatArea) {
                if (chatAreaStackedWidget) {
                    chatAreaStackedWidget->removeWidget(chatArea);
                }
                delete chatArea;
            }
            delete channelItem;
        }
        delete category;
    }
}

QTreeWidgetItem* ChannelTree::createCategoryItem(TeamItem& teamItem, const QString& categoryId,
                                                 const QString& displayName, bool collapsed)
{
    auto* item = new QTreeWidgetItem(&teamItem, QStringList() << displayName);
    item->setData(0, ItemKindRole, CategoryItemKind);
    item->setData(0, ItemIdRole, categoryId);
    item->setData(0, ItemTeamIdRole, teamItem.teamId);
    item->setFlags((item->flags() | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled)
                   & ~Qt::ItemIsEditable);

    QFont font = item->font(0);
    font.setBold(true);
    font.setPixelSize(12);
    item->setFont(0, font);
    item->setSizeHint(0, QSize(0, 22));
    item->setExpanded(!collapsed);
    return item;
}

ChannelItem* ChannelTree::createPersonalItem(Backend& backend, TeamItem& teamItem,
                                             QTreeWidgetItem& categoryItem)
{
    auto* item = new DirectChannelItem(backend, nullptr);
    categoryItem.addChild(item);
    item->setData(0, ItemKindRole, VirtualDestinationItemKind);
    item->setData(0, ItemIdRole, QStringLiteral("virtual:personal"));
    item->setData(0, ItemTeamIdRole, teamItem.teamId);
    item->setData(0, ItemDestinationRole, SidebarItem::PersonalDestination);
    item->setData(0, ItemChannelTypeRole, BackendChannel::directChannel);
    item->setData(0, Qt::UserRole, QVariant::fromValue(static_cast<ChatArea*>(nullptr)));
    item->setFlags(item->flags()
                   & ~(Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled | Qt::ItemIsEditable));
    item->setLabel(tr("Personal"));

    if (BackendChannel* channel = backend.getStorage().getDirectChannelByUserId(
            backend.getLoginUser().id)) {
        item->setData(0, ItemChannelIdRole, channel->id);
    }

    const BackendUser& self = backend.getLoginUser();
    item->setStatus(self.status);
    if (!self.avatar.isNull()) {
        item->setIcon(QIcon(self.avatar));
    }

    if (!personalUserConnected) {
        personalUserConnected = true;
        if (self.avatar.isNull()) {
            backend.retrieveUserAvatar(self.id);
        }
        connect(&self, &BackendUser::onAvatarChanged, this,
                [this] { refreshPersonalItems(); });
        connect(&self, &BackendUser::onStatusChanged, this,
                [this] { refreshPersonalItems(); });
    }
    return item;
}

QTreeWidgetItem* ChannelTree::personalItemForTeam(const QString& teamId) const
{
    TeamItem* teamItem = teamToItemMap.value(teamId, nullptr);
    if (!teamItem) {
        return nullptr;
    }

    for (int categoryIndex = 0; categoryIndex < teamItem->childCount(); ++categoryIndex) {
        QTreeWidgetItem* category = teamItem->child(categoryIndex);
        for (int rowIndex = 0; category && rowIndex < category->childCount(); ++rowIndex) {
            QTreeWidgetItem* row = category->child(rowIndex);
            if (row
                && row->data(0, ItemKindRole).toInt() == VirtualDestinationItemKind
                && row->data(0, ItemDestinationRole).toInt() == SidebarItem::PersonalDestination) {
                return row;
            }
        }
    }
    return nullptr;
}

void ChannelTree::refreshPersonalItems()
{
    if (!backendForSidebar) {
        return;
    }

    const BackendUser& self = backendForSidebar->getLoginUser();
    BackendChannel* channel = backendForSidebar->getStorage().getDirectChannelByUserId(self.id);

    for (auto it = teamToItemMap.cbegin(); it != teamToItemMap.cend(); ++it) {
        QTreeWidgetItem* row = personalItemForTeam(it.key());
        if (!row) {
            continue;
        }
        auto* personalItem = static_cast<ChannelItem*>(row);
        personalItem->setStatus(self.status);
        personalItem->setIcon(self.avatar.isNull() ? QIcon() : QIcon(self.avatar));
        row->setData(0, ItemChannelIdRole, channel ? channel->id : QString());
    }
}

ChannelItem* ChannelTree::createChannelItem(Backend& backend, TeamItem& teamItem,
                                            QTreeWidgetItem& categoryItem, BackendChannel& channel)
{
    ChannelItem* item = nullptr;
    if (channel.type == BackendChannel::directChannel || channel.type == BackendChannel::groupChannel) {
        item = new DirectChannelItem(backend, nullptr);
    } else {
        item = teamItem.createChannelItem(backend, nullptr);
    }

    categoryItem.addChild(item);
    item->setData(0, ItemKindRole, ChannelItemKind);
    item->setData(0, ItemIdRole, channel.id);
    item->setData(0, ItemTeamIdRole, teamItem.teamId);
    item->setData(0, Qt::UserRole, QVariant::fromValue(static_cast<ChatArea*>(nullptr)));
    item->setFlags((item->flags() | Qt::ItemIsDragEnabled) & ~Qt::ItemIsDropEnabled);
    item->setLabel(channel.display_name);

    if (channel.type == BackendChannel::groupChannel) {
        item->setIcon(ChannelIcons::groupConversation());
    } else if (channel.type != BackendChannel::directChannel) {
        item->setIcon(ChannelIcons::channel());
    }

    if (channel.type == BackendChannel::directChannel) {
        BackendUser* user = backend.getStorage().getUserById(channel.name);
        if (user) {
            item->setStatus(user->status);
            if (!user->avatar.isNull()) {
                item->setIcon(QIcon(user->avatar));
            }

            if (!connectedSidebarUsers.contains(user->id)) {
                connectedSidebarUsers.insert(user->id);

                // Avatars are low-priority requests and are now limited to the
                // actually visible DM set rather than the entire user directory.
                if (user->avatar.isNull()) {
                    backend.retrieveUserAvatar(user->id);
                }

                connect(user, &BackendUser::onStatusChanged, this, [this, user] {
                    if (!backendForSidebar) {
                        return;
                    }
                    BackendChannel* directChannel = backendForSidebar->getStorage().getDirectChannelByUserId(user->id);
                    if (!directChannel) {
                        return;
                    }
                    const auto items = channelToItemMap.value(directChannel->id);
                    for (QTreeWidgetItem* row : items) {
                        if (row && row->data(0, ItemKindRole).toInt() == ChannelItemKind) {
                            static_cast<ChannelItem*>(row)->setStatus(user->status);
                        }
                    }
                });

                connect(user, &BackendUser::onAvatarChanged, this, [this, user] {
                    if (!backendForSidebar) {
                        return;
                    }
                    BackendChannel* directChannel = backendForSidebar->getStorage().getDirectChannelByUserId(user->id);
                    if (!directChannel) {
                        return;
                    }
                    const auto items = channelToItemMap.value(directChannel->id);
                    for (QTreeWidgetItem* row : items) {
                        if (row && row->data(0, ItemKindRole).toInt() == ChannelItemKind) {
                            static_cast<ChannelItem*>(row)->setIcon(QIcon(user->avatar));
                        }
                    }
                });
            }
        }
    }

    // A channel may appear in multiple sidebar categories and category refreshes
    // routinely destroy/recreate the QTreeWidgetItem objects. Never capture a
    // row pointer in the long-lived BackendChannel::onLeave connection. The
    // member slot resolves the currently alive rows from channelToItemMap.
    QObject::connect(&channel, &BackendChannel::onLeave,
                     this, &ChannelTree::handleChannelLeave,
                     Qt::UniqueConnection);

    addChannelToItem(channel.id, item);
    auto& sidebar = SidebarService::instance(backend);
    item->setMuted(sidebar.isChannelMuted(channel));
    item->setMentioned(sidebar.hasUnreadMention(channel.id));
    return item;
}

void ChannelTree::handleChannelLeave()
{
    auto* channel = qobject_cast<BackendChannel*>(sender());
    if (!channel) {
        return;
    }

    const QString channelId = channel->id;
    const QList<QTreeWidgetItem*> items = channelToItemMap.value(channelId);
    for (QTreeWidgetItem* item : items) {
        if (!item || item->data(0, ItemKindRole).toInt() != ChannelItemKind) {
            continue;
        }

        removeChannelToItem(channelId, item);
        ChatArea* chatArea = item->data(0, Qt::UserRole).value<ChatArea*>();
        if (chatArea && chatAreaStackedWidget) {
            chatAreaStackedWidget->removeWidget(chatArea);
        }
        if (QTreeWidgetItem* parent = item->parent()) {
            parent->removeChild(item);
        }
        delete chatArea;
        delete item;
    }

    channelToItemMap.remove(channelId);
}

ChatArea* ChannelTree::ensureChatArea(QTreeWidgetItem* item)
{
    if (!item || !backendForSidebar || !chatAreaStackedWidget) {
        return nullptr;
    }

    const int kind = item->data(0, ItemKindRole).toInt();
    if (kind != ChannelItemKind && kind != VirtualDestinationItemKind) {
        return nullptr;
    }

    if (ChatArea* existing = item->data(0, Qt::UserRole).value<ChatArea*>()) {
        return existing;
    }

    QString channelId = kind == ChannelItemKind
        ? item->data(0, ItemIdRole).toString()
        : item->data(0, ItemChannelIdRole).toString();
    if (channelId.isEmpty() && kind == VirtualDestinationItemKind
        && item->data(0, ItemDestinationRole).toInt() == SidebarItem::PersonalDestination) {
        if (BackendChannel* personal = backendForSidebar->getStorage().getDirectChannelByUserId(
                backendForSidebar->getLoginUser().id)) {
            channelId = personal->id;
            item->setData(0, ItemChannelIdRole, channelId);
        }
    }
    BackendChannel* channel = backendForSidebar->getStorage().getChannelById(channelId);
    if (!channel) {
        LOG_DEBUG("Cannot create chat area: channel not found: " << channelId);
        return nullptr;
    }

    auto* channelItem = static_cast<ChannelItem*>(item);
    auto* chatArea = new ChatArea(*backendForSidebar, *channel, channelItem,
                                  chatAreaStackedWidget, false);
    chatAreaStackedWidget->addWidget(chatArea);
    item->setData(0, Qt::UserRole, QVariant::fromValue(chatArea));
    return chatArea;
}

void ChannelTree::activateChannelItem(QTreeWidgetItem* item)
{
    if (!item) {
        return;
    }

    const int kind = item->data(0, ItemKindRole).toInt();
    if (kind == VirtualDestinationItemKind) {
        activateVirtualDestination(item);
        return;
    }
    if (kind != ChannelItemKind) {
        return;
    }

    if (backendForSidebar) {
        BackendChannel* personal = backendForSidebar->getStorage().getDirectChannelByUserId(
            backendForSidebar->getLoginUser().id);
        if (personal && item->data(0, ItemIdRole).toString() == personal->id) {
            if (QTreeWidgetItem* alias = personalItemForTeam(
                    item->data(0, ItemTeamIdRole).toString())) {
                setCurrentItem(alias);
                return;
            }
        }
    }

    ChatArea* newPage = ensureChatArea(item);
    if (!newPage) {
        return;
    }

    if (newPage == getCurrentPage()) {
        if (!newPage->isVisible()) {
            chatAreaStackedWidget->setCurrentWidget(newPage);
        }

        // QStackedWidget automatically makes its first added widget current.
        // With lazy ChatArea creation that means the first selected channel can
        // already be getCurrentPage() even though onActivate()/init() have never
        // run. Activation is idempotent, so always establish the backend current
        // channel and initialize the page on an explicit channel activation.
        newPage->onActivate();
        qDebug() << "Item Activated: " << newPage->channel.display_name;
        return;
    }

    if (ChatArea* currentPage = getCurrentPage()) {
        currentPage->onDeactivate();
    }
    chatAreaStackedWidget->setCurrentWidget(newPage);
    newPage->onActivate();

    qDebug() << "Item Activated: " << newPage->channel.display_name;
}

void ChannelTree::activateVirtualDestination(QTreeWidgetItem* item)
{
    if (!item || !backendForSidebar
        || item->data(0, ItemKindRole).toInt() != VirtualDestinationItemKind
        || item->data(0, ItemDestinationRole).toInt() != SidebarItem::PersonalDestination) {
        return;
    }

    BackendChannel* channel = backendForSidebar->getStorage().getDirectChannelByUserId(
        backendForSidebar->getLoginUser().id);
    if (!channel) {
        const QString teamId = item->data(0, ItemTeamIdRole).toString();
        QPointer<ChannelTree> guard(this);
        backendForSidebar->createDirectChannel(backendForSidebar->getLoginUser(),
            [guard, teamId](BackendChannel& created) {
                if (!guard) {
                    return;
                }
                QTreeWidgetItem* currentPersonal = guard->personalItemForTeam(teamId);
                if (!currentPersonal) {
                    return;
                }
                currentPersonal->setData(0, ItemChannelIdRole, created.id);
                guard->refreshPersonalItems();
                // Do not steal focus if the user navigated elsewhere while the
                // first-ever self-DM was being created.
                if (guard->currentItem() == currentPersonal) {
                    guard->activateVirtualDestination(currentPersonal);
                }
            });
        return;
    }

    item->setData(0, ItemChannelIdRole, channel->id);
    ChatArea* newPage = ensureChatArea(item);
    if (!newPage) {
        return;
    }

    if (newPage == getCurrentPage()) {
        if (!newPage->isVisible()) {
            chatAreaStackedWidget->setCurrentWidget(newPage);
        }
        newPage->onActivate();
        return;
    }

    if (ChatArea* currentPage = getCurrentPage()) {
        currentPage->onDeactivate();
    }
    chatAreaStackedWidget->setCurrentWidget(newPage);
    newPage->onActivate();
}

void ChannelTree::addGroupChannelsList (Backend& backend)
{
    Q_UNUSED(backend);
}

void ChannelTree::addDirectChannelsList (Backend& backend)
{
    Q_UNUSED(backend);
}

void ChannelTree::setChatAreaStackedWidget (QStackedWidget* stackedWidget)
{
	chatAreaStackedWidget = stackedWidget;
}

void ChannelTree::openChannel (QString channelID)
{
	auto it = channelToItemMap.find (channelID);
    QTreeWidgetItem* item = nullptr;

    if (it != channelToItemMap.end() && !it.value().isEmpty()) {
        item = it.value().front();
    } else if (backendForSidebar) {
        BackendChannel* personal = backendForSidebar->getStorage().getDirectChannelByUserId(
            backendForSidebar->getLoginUser().id);
        if (personal && personal->id == channelID) {
            // Programmatic navigation (permalinks, Saved/Search results, etc.)
            // must resolve the self-DM even when its ordinary server row is
            // absent or suppressed in Favorites. Prefer the currently selected
            // team's Personal row, otherwise use the first available one.
            QTreeWidgetItem* current = currentItem();
            QTreeWidgetItem* teamItem = current;
            while (teamItem && teamItem->parent()) {
                teamItem = teamItem->parent();
            }
            if (teamItem && teamItem->data(0, ItemKindRole).toInt() == TeamItemKind) {
                item = personalItemForTeam(teamItem->data(0, ItemTeamIdRole).toString());
            }
            if (!item) {
                for (auto teamIt = teamToItemMap.cbegin(); teamIt != teamToItemMap.cend(); ++teamIt) {
                    item = personalItemForTeam(teamIt.key());
                    if (item) {
                        break;
                    }
                }
            }
        }
    }

    if (!item) {
        qDebug() << "openChannel " << channelID << ": channel not found";
        return;
    }

    if (currentItem() == item) {
        activateChannelItem(item);
    } else {
        setCurrentItem(item);
    }
}

void ChannelTree::addChannelToItem (QString channelID, QTreeWidgetItem* item)
{
	auto& items = channelToItemMap[channelID];
	if (!items.contains(item)) {
		items.push_back(item);
	}
}

void ChannelTree::removeChannelToItem (QString channelID, QTreeWidgetItem* item)
{
	auto it = channelToItemMap.find(channelID);
	if (it == channelToItemMap.end()) {
		return;
	}

	if (item) {
		it.value().removeAll(item);
	} else {
		it.value().clear();
	}

	if (it.value().isEmpty()) {
		channelToItemMap.erase(it);
	}
}

void ChannelTree::showContextMenu (const QPoint& pos)
{
	QTreeWidgetItem* item = itemAt(pos);
    if (!item || item->data(0, ItemKindRole).toInt() == CategoryItemKind) {
		return;
	}

    const int kind = item->data(0, ItemKindRole).toInt();
    if (kind != TeamItemKind && kind != ChannelItemKind) {
        return;
    }

    auto* pointedItem = static_cast<ChannelTreeItem*>(item);
	pointedItem->showContextMenu (mapToGlobal(pos) + QPoint (25, 15));
}

ChatArea* ChannelTree::getCurrentPage ()
{
    if (!chatAreaStackedWidget) {
        return nullptr;
    }
	return qobject_cast<ChatArea*> (chatAreaStackedWidget->currentWidget());
}

void ChannelTree::setCategoryCollapsed(QTreeWidgetItem* item, bool collapsed)
{
    if (!backendForSidebar || !item || item->data(0, ItemKindRole).toInt() != CategoryItemKind) {
        return;
    }

    const QString teamId = item->data(0, ItemTeamIdRole).toString();
    const QString categoryId = item->data(0, ItemIdRole).toString();
    SidebarTeamState* state = SidebarService::instance(*backendForSidebar).teamState(teamId);
    SidebarCategory* category = state ? state->category(categoryId) : nullptr;
    if (!category || category->collapsed == collapsed) {
        return;
    }

    category->collapsed = collapsed;
    SidebarService::instance(*backendForSidebar).updateCategory(*category);
}

void ChannelTree::setChannelMutedVisual(const QString& channelId, bool muted)
{
    const auto items = channelToItemMap.value(channelId);
    for (QTreeWidgetItem* item : items) {
        if (item && item->data(0, ItemKindRole).toInt() == ChannelItemKind) {
            static_cast<ChannelItem*>(item)->setMuted(muted);
        }
    }
}

void ChannelTree::setChannelMentionedVisual(const QString& channelId, bool mentioned)
{
    const auto items = channelToItemMap.value(channelId);
    for (QTreeWidgetItem* item : items) {
        if (item && item->data(0, ItemKindRole).toInt() == ChannelItemKind) {
            static_cast<ChannelItem*>(item)->setMentioned(mentioned);
        }
    }
}

bool ChannelTree::canRemoveChannelFromCategory(const ChannelItem* item) const
{
    if (!backendForSidebar || !item || !item->parent()) {
        return false;
    }

    QTreeWidgetItem* categoryItem = item->parent();
    if (categoryItem->data(0, ItemKindRole).toInt() != CategoryItemKind) {
        return false;
    }

    const QString teamId = categoryItem->data(0, ItemTeamIdRole).toString();
    const QString categoryId = categoryItem->data(0, ItemIdRole).toString();
    const SidebarTeamState* state = SidebarService::instance(*backendForSidebar).teamState(teamId);
    const SidebarCategory* category = state ? state->category(categoryId) : nullptr;
    return category && (category->type == QStringLiteral("custom")
                        || category->type == QStringLiteral("favorites"));
}

void ChannelTree::removeChannelFromCategory(ChannelItem* item)
{
    if (!canRemoveChannelFromCategory(item) || !backendForSidebar) {
        return;
    }

    QTreeWidgetItem* sourceCategoryItem = item->parent();
    QTreeWidgetItem* teamItem = sourceCategoryItem ? sourceCategoryItem->parent() : nullptr;
    const QString channelId = item->data(0, ItemIdRole).toString();
    BackendChannel* channel = backendForSidebar->getStorage().getChannelById(channelId);
    if (!teamItem || !channel) {
        return;
    }

    const QString teamId = sourceCategoryItem->data(0, ItemTeamIdRole).toString();
    SidebarTeamState* state = SidebarService::instance(*backendForSidebar).teamState(teamId);
    SidebarCategory* sourceCategory = state
        ? state->category(sourceCategoryItem->data(0, ItemIdRole).toString()) : nullptr;
    if (!sourceCategory) {
        return;
    }

    const bool direct = channel->type == BackendChannel::directChannel
        || channel->type == BackendChannel::groupChannel;
    SidebarCategory* targetCategory = state->categoryByType(
        direct ? QStringLiteral("direct_messages") : QStringLiteral("channels"));
    if (!targetCategory || targetCategory->id == sourceCategory->id) {
        return;
    }

    sourceCategory->channelIds.removeAll(channelId);
    targetCategory->channelIds.removeAll(channelId);
    targetCategory->channelIds.push_back(channelId);

    QVector<SidebarCategory> updates {*sourceCategory, *targetCategory};
    SidebarService::instance(*backendForSidebar).updateCategories(teamId, updates,
        [this, teamId](const SidebarTeamState&) {
            if (!backendForSidebar) {
                return;
            }
            BackendTeam* team = backendForSidebar->getStorage().getTeamById(teamId);
            if (team) {
                refreshTeamSidebar(*backendForSidebar, *team);
            }
        });
}

void ChannelTree::dropEvent(QDropEvent* event)
{
    const QPoint eventPosition = dropEventPosition(event);
    QTreeWidgetItem* source = currentItem();
    QTreeWidgetItem* target = itemAt(eventPosition);
    if (!source || !target || source == target) {
        event->ignore();
        return;
    }

    if (source->data(0, ItemKindRole).toInt() != ChannelItemKind) {
        event->ignore();
        return;
    }

    QTreeWidgetItem* targetCategoryItem = target;
    if (target->data(0, ItemKindRole).toInt() == ChannelItemKind) {
        targetCategoryItem = target->parent();
    }
    if (!targetCategoryItem || targetCategoryItem->data(0, ItemKindRole).toInt() != CategoryItemKind) {
        event->ignore();
        return;
    }

    QTreeWidgetItem* sourceCategoryItem = source->parent();
    QTreeWidgetItem* teamItem = sourceCategoryItem ? sourceCategoryItem->parent() : nullptr;
    if (!sourceCategoryItem || !teamItem || targetCategoryItem->parent() != teamItem) {
        event->ignore();
        return;
    }

    const QString teamId = targetCategoryItem->data(0, ItemTeamIdRole).toString();
    SidebarTeamState* state = backendForSidebar
        ? SidebarService::instance(*backendForSidebar).teamState(teamId)
        : nullptr;
    SidebarCategory* sourceCategory = state
        ? state->category(sourceCategoryItem->data(0, ItemIdRole).toString()) : nullptr;
    SidebarCategory* targetCategory = state
        ? state->category(targetCategoryItem->data(0, ItemIdRole).toString()) : nullptr;
    if (!sourceCategory || !targetCategory) {
        event->ignore();
        return;
    }

    const QString channelId = source->data(0, ItemIdRole).toString();
    if (channelId.isEmpty()) {
        event->ignore();
        return;
    }

    if (sourceCategory->id == targetCategory->id) {
        const int oldIndex = sourceCategory->channelIds.indexOf(channelId);
        int newIndex = targetCategoryItem->indexOfChild(target);
        if (oldIndex < 0 || newIndex < 0) {
            event->ignore();
            return;
        }
        sourceCategory->channelIds.removeAt(oldIndex);
        if (newIndex > oldIndex) {
            --newIndex;
        }
        sourceCategory->channelIds.insert(newIndex, channelId);
        SidebarService::instance(*backendForSidebar).updateCategory(*sourceCategory);
    } else {
        sourceCategory->channelIds.removeAll(channelId);
        targetCategory->channelIds.removeAll(channelId);
        int insertIndex = targetCategoryItem->indexOfChild(target);
        if (insertIndex < 0 || insertIndex > targetCategory->channelIds.size()) {
            insertIndex = targetCategory->channelIds.size();
        }
        targetCategory->channelIds.insert(insertIndex, channelId);

        QVector<SidebarCategory> updates {*sourceCategory, *targetCategory};
        SidebarService::instance(*backendForSidebar).updateCategories(teamId, updates);
    }

    event->acceptProposedAction();
    if (backendForSidebar) {
        if (BackendTeam* team = backendForSidebar->getStorage().getTeamById(teamId)) {
            refreshTeamSidebar(*backendForSidebar, *team);
        }
    }
}

} // namespace Mattermost
