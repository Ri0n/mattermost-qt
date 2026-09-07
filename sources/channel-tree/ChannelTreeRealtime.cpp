/**
 * @file ChannelTreeRealtime.cpp
 * @brief Reconcile realtime DM/GM channels with the server-backed sidebar.
 *
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#include "ChannelTree.h"

#include "backend/Backend.h"
#include "backend/SidebarService.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "channel-tree/team-item/TeamItem.h"

namespace Mattermost {

void ChannelTree::admitStoredConversation(BackendChannel& channel)
{
    if (!backendForSidebar
        || (channel.type != BackendChannel::directChannel
            && channel.type != BackendChannel::groupChannel)) {
        return;
    }

    auto& sidebar = SidebarService::instance(*backendForSidebar);

    // Direct/group conversations are global Mattermost conversations but the
    // sidebar exposes them through each team's server-backed Direct Messages
    // category. A direct_added event can make the BackendChannel available
    // before that category is refreshed, so materialize the row locally and
    // let the next authoritative category response reconcile ordering/state.
    for (auto teamIt = teamToItemMap.begin(); teamIt != teamToItemMap.end(); ++teamIt) {
        TeamItem* teamItem = teamIt.value();
        SidebarTeamState* state = sidebar.teamState(teamIt.key());
        SidebarCategory* category = state
            ? state->categoryByType(QStringLiteral("direct_messages")) : nullptr;
        if (!teamItem || !category) {
            continue;
        }

        if (!category->channelIds.contains(channel.id)) {
            category->channelIds.prepend(channel.id);
        }

        QTreeWidgetItem* categoryItem = nullptr;
        for (int index = 0; index < teamItem->childCount(); ++index) {
            QTreeWidgetItem* candidate = teamItem->child(index);
            if (candidate
                && candidate->data(0, ItemKindRole).toInt() == CategoryItemKind
                && candidate->data(0, ItemIdRole).toString() == category->id) {
                categoryItem = candidate;
                break;
            }
        }
        if (!categoryItem) {
            continue;
        }

        bool alreadyMaterialized = false;
        for (int index = 0; index < categoryItem->childCount(); ++index) {
            QTreeWidgetItem* row = categoryItem->child(index);
            if (row
                && row->data(0, ItemKindRole).toInt() == ChannelItemKind
                && row->data(0, ItemIdRole).toString() == channel.id) {
                alreadyMaterialized = true;
                break;
            }
        }
        if (!alreadyMaterialized) {
            createChannelItem(*backendForSidebar, *teamItem, *categoryItem, channel);
        }
    }
}

void ChannelTree::openStoredChannel(QString channelID)
{
    if (channelID.isEmpty()) {
        return;
    }

    const auto existing = channelToItemMap.constFind(channelID);
    if (existing == channelToItemMap.cend() || existing.value().isEmpty()) {
        BackendChannel* channel = backendForSidebar
            ? backendForSidebar->getStorage().getChannelById(channelID) : nullptr;
        if (channel) {
            admitStoredConversation(*channel);
        }
    }

    openChannel(channelID);
}

} // namespace Mattermost
