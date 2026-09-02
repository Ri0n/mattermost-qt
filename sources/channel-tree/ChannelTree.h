/**
 * @file ChannelTree.h
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

#pragma once

#include <QMap>
#include <QSet>
#include <QTreeWidget>

#include "ChannelTreeItem.h"
#include "SidebarItem.h"

class QDropEvent;
class QMouseEvent;
class QStackedWidget;
class QTreeWidgetItem;

namespace Mattermost {

class Backend;
class BackendChannel;
class BackendTeam;
class ChatArea;
class TeamItem;
class ChannelItem;
struct SidebarTeamState;

class ChannelTree: public QTreeWidget {
	Q_OBJECT
public:
    using ItemKind = SidebarItem::Kind;
    using ItemRole = SidebarItem::Role;

    // Compatibility names for the existing ChannelTree implementation and
    // callers. SidebarItem is the canonical contract shared by all sidebar
    // views; new code should use it directly.
    enum : int {
        UnknownItem = SidebarItem::Unknown,
        TeamItemKind = SidebarItem::Team,
        CategoryItemKind = SidebarItem::Category,
        ChannelItemKind = SidebarItem::Channel,
    };

    enum : int {
        ItemKindRole = SidebarItem::KindRole,
        ItemIdRole = SidebarItem::IdRole,
        ItemTeamIdRole = SidebarItem::TeamIdRole,
        ItemMutedRole = SidebarItem::MutedRole,
        ItemMentionedRole = SidebarItem::MentionedRole,
        ItemStatusRole = SidebarItem::PresenceRole,
        ItemUnreadRole = SidebarItem::UnreadRole,
        ItemLifetimeRole = SidebarItem::LifetimeRole,
        ItemChannelTypeRole = SidebarItem::ChannelTypeRole,
    };

	ChannelTree (QWidget* parent = nullptr);
	virtual ~ChannelTree ();
public:
	bool isChannelActive (const BackendChannel& channel);
	Backend* backendInstance() const { return backendForSidebar; }

	void addTeam (Backend& backend, BackendTeam& team);
	void populateSidebars(Backend& backend);

	// Kept for source compatibility; the server-backed sidebar no longer uses
	// separate global DM/GM lists.
	void addDirectChannelsList (Backend& backend);
	void addGroupChannelsList (Backend& backend);

	void setChatAreaStackedWidget (QStackedWidget* chatAreaStackedWidget);
	ChatArea* getCurrentPage ();

	void openChannel (QString channelID);
	void addChannelToItem (QString channelID, QTreeWidgetItem* item);
	void removeChannelToItem (QString channelID, QTreeWidgetItem* item = nullptr);

	// Recent and Attention are alternate views over the same channel objects.
	// Route their context-menu requests back through the real ChannelItem so all
	// actions and handlers stay identical to the Channels tab.
	void showChannelContextMenu(const QString& channelID, const QPoint& globalPos)
	{
		auto it = channelToItemMap.constFind(channelID);
		if (it == channelToItemMap.cend()) {
			return;
		}
		for (QTreeWidgetItem* item : it.value()) {
			if (!item || item->data(0, ItemKindRole).toInt() != ChannelItemKind) {
				continue;
			}
			static_cast<ChannelTreeItem*>(item)->showContextMenu(globalPos);
			return;
		}
	}

	bool canRemoveChannelFromCategory(const ChannelItem* item) const;
	void removeChannelFromCategory(ChannelItem* item);

protected:
	void currentChanged(const QModelIndex& current, const QModelIndex& previous) override;
	void mousePressEvent(QMouseEvent* event) override;
	void dropEvent(QDropEvent* event) override;

private:
	void markChannelViewed(QTreeWidgetItem* item);
	void showContextMenu (const QPoint& pos);
	void handleChannelLeave();
	void refreshTeamSidebar(Backend& backend, BackendTeam& team);
	void renderTeamSidebar(Backend& backend, TeamItem& teamItem,
	                       const SidebarTeamState& state);
	void clearTeamSidebar(TeamItem& teamItem);
	QTreeWidgetItem* createCategoryItem(TeamItem& teamItem, const QString& categoryId,
	                                    const QString& displayName, bool collapsed);
	ChannelItem* createChannelItem(Backend& backend, TeamItem& teamItem,
	                               QTreeWidgetItem& categoryItem, BackendChannel& channel);
	ChatArea* ensureChatArea(QTreeWidgetItem* item);
	void activateChannelItem(QTreeWidgetItem* item);
	void setCategoryCollapsed(QTreeWidgetItem* item, bool collapsed);
	void setChannelMutedVisual(const QString& channelId, bool muted);
	void setChannelMentionedVisual(const QString& channelId, bool mentioned);
	void syncCategoryChannels(QTreeWidgetItem* firstCategory, QTreeWidgetItem* secondCategory = nullptr);
	void syncCategoryOrder(QTreeWidgetItem* teamItem);
	QStringList channelIds(QTreeWidgetItem* categoryItem) const;

	QStackedWidget*						chatAreaStackedWidget;
	QMap<QString, QList<QTreeWidgetItem*>>	channelToItemMap;
	QMap<QString, TeamItem*>			teamToItemMap;
	QSet<QString>						connectedSidebarUsers;
	Backend*							backendForSidebar;
	bool							renderingSidebar;
};

} /* namespace Mattermost */
