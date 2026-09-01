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
 * the Free Software Foundation; either version 3 of the License, or
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
#include <QStackedWidget>

#include "backend/Backend.h"
#include "backend/SidebarService.h"
#include "backend/types/BackendTeam.h"
#include "chat-area/ChatArea.h"
#include "channel-tree/ChannelItem.h"
#include "channel-tree/ChannelItemWidget.h"
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

	connect (this, &QTreeWidget::currentItemChanged, [this] (QTreeWidgetItem* item, QTreeWidgetItem*) {
        if (!item || item->data(0, ItemKindRole).toInt() != ChannelItemKind) {
            return;
        }

		ChatArea *newPage = item->data(0, Qt::UserRole).value<ChatArea*>();
		if (!newPage) {
			return;
		}

		if (newPage == getCurrentPage ()) {
			return;
		}

        if (ChatArea* currentPage = getCurrentPage()) {
            currentPage->onDeactivate ();
        }
		chatAreaStackedWidget->setCurrentWidget (newPage);
		newPage->onActivate ();

		qDebug() << "Item Activated: " << newPage->channel.display_name;
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

    // Populate Storage first. The category renderer runs after all teams have
    // completed, because DM/GM channels are returned in every team's channel list.
	backend.retrieveOwnChannelMembershipsForTeam (team, [] (BackendChannel&) {});
	backend.retrieveTeamMembers (team);
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

    for (const auto& categoryId : state.order) {
        const SidebarCategory* category = state.category(categoryId);
        if (!category) {
            continue;
        }

        QTreeWidgetItem* categoryItem = createCategoryItem(
            teamItem, category->id, categoryDisplayName(*category), category->collapsed);

        for (const auto& channelId : category->channelIds) {
            BackendChannel* channel = backend.getStorage().getChannelById(channelId);
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
            ChatArea* chatArea = channelItem->data(0, Qt::UserRole).value<ChatArea*>();
            if (chatArea) {
                removeChannelToItem(chatArea->channel.id, channelItem);
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
    item->setExpanded(!collapsed);
    return item;
}

ChannelItem* ChannelTree::createChannelItem(Backend& backend, TeamItem& teamItem,
                                            QTreeWidgetItem& categoryItem, BackendChannel& channel)
{
    auto* itemWidget = new ChannelItemWidget(this);
    itemWidget->setLabel(channel.display_name);

    ChannelItem* item = nullptr;
    if (channel.type == BackendChannel::directChannel || channel.type == BackendChannel::groupChannel) {
        item = new DirectChannelItem(backend, itemWidget);
    } else {
        item = teamItem.createChannelItem(backend, itemWidget);
    }

    categoryItem.addChild(item);
    item->setData(0, ItemKindRole, ChannelItemKind);
    item->setData(0, ItemIdRole, channel.id);
    item->setData(0, ItemTeamIdRole, teamItem.teamId);
    item->setFlags((item->flags() | Qt::ItemIsDragEnabled) & ~Qt::ItemIsDropEnabled);

    ChatArea* chatArea = new ChatArea(backend, channel, item, chatAreaStackedWidget, false);
    chatAreaStackedWidget->addWidget(chatArea);
    item->setData(0, Qt::UserRole, QVariant::fromValue(chatArea));
    setItemWidget(item, 0, itemWidget);

    QObject::connect(&channel, &BackendChannel::onLeave, chatArea,
                     [this, &channel, item, chatArea] {
        removeChannelToItem(channel.id, item);
        if (chatAreaStackedWidget) {
            chatAreaStackedWidget->removeWidget(chatArea);
        }
        if (QTreeWidgetItem* parent = item->parent()) {
            parent->removeChild(item);
        }
        delete item;
        delete chatArea;
    });

    addChannelToItem(channel.id, item);
    auto& sidebar = SidebarService::instance(backend);
    item->setMuted(sidebar.isChannelMuted(channel));
    item->setMentioned(sidebar.hasUnreadMention(channel.id));
    return item;
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
	if (it == channelToItemMap.end() || it.value().isEmpty()) {
		qDebug() << "openChannel " << channelID << ": channel not found";
		return;
	}

	setCurrentItem (it.value().front());
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
	return static_cast<ChatArea*> (chatAreaStackedWidget->currentWidget());
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
    QTreeWidgetItem* teamItem = sourceCategoryItem->parent();
    ChatArea* chatArea = item->data(0, Qt::UserRole).value<ChatArea*>();
    if (!teamItem || !chatArea) {
        return;
    }

    const QString teamId = sourceCategoryItem->data(0, ItemTeamIdRole).toString();
    SidebarTeamState* state = SidebarService::instance(*backendForSidebar).teamState(teamId);
    SidebarCategory* sourceCategory = state
        ? state->category(sourceCategoryItem->data(0, ItemIdRole).toString()) : nullptr;
    if (!sourceCategory) {
        return;
    }

    const bool direct = chatArea->channel.type == BackendChannel::directChannel
        || chatArea->channel.type == BackendChannel::groupChannel;
    SidebarCategory* targetCategory = state->categoryByType(
        direct ? QStringLiteral("direct_messages") : QStringLiteral("channels"));
    if (!targetCategory || targetCategory->id == sourceCategory->id) {
        return;
    }

    sourceCategory->channelIds.removeAll(chatArea->channel.id);
    targetCategory->channelIds.removeAll(chatArea->channel.id);
    targetCategory->channelIds.push_back(chatArea->channel.id);

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
    QTreeWidgetItem* source = currentItem();
    QTreeWidgetItem* target = itemAt(dropEventPosition(event));
    if (!source || !target || source == target) {
        event->ignore();
        return;
    }

    const int sourceKind = source->data(0, ItemKindRole).toInt();
    const int targetKind = target->data(0, ItemKindRole).toInt();
    const auto indicator = dropIndicatorPosition();

    if (sourceKind == CategoryItemKind) {
        QTreeWidgetItem* sourceTeam = source->parent();
        if (!sourceTeam) {
            event->ignore();
            return;
        }

        if (targetKind == ChannelItemKind) {
            target = target->parent();
        }

        if (target->data(0, ItemKindRole).toInt() == CategoryItemKind) {
            if (target->parent() != sourceTeam || indicator == QAbstractItemView::OnItem) {
                event->ignore();
                return;
            }
        } else if (target->data(0, ItemKindRole).toInt() == TeamItemKind) {
            if (target != sourceTeam || indicator != QAbstractItemView::OnItem) {
                event->ignore();
                return;
            }
        } else {
            event->ignore();
            return;
        }

        QTreeWidget::dropEvent(event);
        syncCategoryOrder(sourceTeam);
        return;
    }

    if (sourceKind == ChannelItemKind) {
        QTreeWidgetItem* oldCategory = source->parent();
        QTreeWidgetItem* sourceTeam = oldCategory ? oldCategory->parent() : nullptr;
        QTreeWidgetItem* targetCategory = nullptr;

        if (targetKind == CategoryItemKind) {
            if (indicator != QAbstractItemView::OnItem) {
                event->ignore();
                return;
            }
            targetCategory = target;
        } else if (targetKind == ChannelItemKind) {
            if (indicator == QAbstractItemView::OnItem) {
                event->ignore();
                return;
            }
            targetCategory = target->parent();
        }

        if (!oldCategory || !sourceTeam || !targetCategory || targetCategory->parent() != sourceTeam) {
            event->ignore();
            return;
        }

        QTreeWidget::dropEvent(event);
        QTreeWidgetItem* newCategory = source->parent();
        if (!newCategory || newCategory->data(0, ItemKindRole).toInt() != CategoryItemKind) {
            event->ignore();
            refreshTeamSidebar(*backendForSidebar,
                               *backendForSidebar->getStorage().getTeamById(
                                   sourceTeam->data(0, ItemTeamIdRole).toString()));
            return;
        }

        syncCategoryChannels(oldCategory, newCategory);
        return;
    }

    event->ignore();
}

void ChannelTree::syncCategoryChannels(QTreeWidgetItem* firstCategory, QTreeWidgetItem* secondCategory)
{
    if (!backendForSidebar || !firstCategory) {
        return;
    }

    const QString teamId = firstCategory->data(0, ItemTeamIdRole).toString();
    SidebarTeamState* state = SidebarService::instance(*backendForSidebar).teamState(teamId);
    if (!state) {
        return;
    }

    QVector<SidebarCategory> updates;
    const auto addCategoryUpdate = [this, state, &updates](QTreeWidgetItem* categoryItem) {
        if (!categoryItem) {
            return;
        }
        SidebarCategory* category = state->category(categoryItem->data(0, ItemIdRole).toString());
        if (!category) {
            return;
        }
        category->channelIds = channelIds(categoryItem);
        updates.push_back(*category);
    };

    addCategoryUpdate(firstCategory);
    if (secondCategory && secondCategory != firstCategory) {
        addCategoryUpdate(secondCategory);
    }

    if (!updates.isEmpty()) {
        SidebarService::instance(*backendForSidebar).updateCategories(teamId, updates);
    }
}

void ChannelTree::syncCategoryOrder(QTreeWidgetItem* teamItem)
{
    if (!backendForSidebar || !teamItem) {
        return;
    }

    QStringList order;
    for (int i = 0; i < teamItem->childCount(); ++i) {
        QTreeWidgetItem* category = teamItem->child(i);
        if (category->data(0, ItemKindRole).toInt() == CategoryItemKind) {
            order.push_back(category->data(0, ItemIdRole).toString());
        }
    }

    const QString teamId = teamItem->data(0, ItemTeamIdRole).toString();
    if (SidebarTeamState* state = SidebarService::instance(*backendForSidebar).teamState(teamId)) {
        state->order = order;
    }
    SidebarService::instance(*backendForSidebar).updateCategoryOrder(teamId, order);
}

QStringList ChannelTree::channelIds(QTreeWidgetItem* categoryItem) const
{
    QStringList ids;
    if (!categoryItem) {
        return ids;
    }
    for (int i = 0; i < categoryItem->childCount(); ++i) {
        QTreeWidgetItem* item = categoryItem->child(i);
        if (item->data(0, ItemKindRole).toInt() == ChannelItemKind) {
            ids.push_back(item->data(0, ItemIdRole).toString());
        }
    }
    return ids;
}

} /* namespace Mattermost */
