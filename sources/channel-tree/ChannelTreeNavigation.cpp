#include "ChannelTree.h"

#include <QDebug>
#include <QTreeWidgetItem>

#include "backend/Backend.h"
#include "backend/SidebarService.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "channel-tree/ChannelItem.h"
#include "channel-tree/team-item/TeamItem.h"

namespace Mattermost {

void ChannelTree::openStoredChannel(QString channelID)
{
    auto existing = channelToItemMap.constFind(channelID);
    if (existing != channelToItemMap.cend() && !existing.value().isEmpty()) {
        openChannel(std::move(channelID));
        return;
    }

    if (!backendForSidebar) {
        return;
    }

    BackendChannel* channel = backendForSidebar->getStorage().getChannelById(channelID);
    if (!channel) {
        qDebug() << "openStoredChannel" << channelID << ": channel not found in storage";
        return;
    }

    auto& sidebar = SidebarService::instance(*backendForSidebar);

    // Prefer the Direct Messages category when a channel occurs in more than
    // one server category. A materialized current DM is retained by the sidebar
    // policy on subsequent refreshes even when it is outside the normal limit.
    struct Candidate {
        TeamItem* teamItem = nullptr;
        QTreeWidgetItem* categoryItem = nullptr;
        bool directMessages = false;
    };
    Candidate fallback;

    for (auto teamIt = teamToItemMap.cbegin(); teamIt != teamToItemMap.cend(); ++teamIt) {
        TeamItem* teamItem = teamIt.value();
        const SidebarTeamState* state = sidebar.teamState(teamIt.key());
        if (!teamItem || !state) {
            continue;
        }

        for (const QString& categoryId : state->order) {
            const SidebarCategory* category = state->category(categoryId);
            if (!category || !category->channelIds.contains(channelID)) {
                continue;
            }

            QTreeWidgetItem* categoryItem = nullptr;
            for (int i = 0; i < teamItem->childCount(); ++i) {
                QTreeWidgetItem* child = teamItem->child(i);
                if (child && child->data(0, ItemKindRole).toInt() == CategoryItemKind
                    && child->data(0, ItemIdRole).toString() == categoryId) {
                    categoryItem = child;
                    break;
                }
            }
            if (!categoryItem) {
                continue;
            }

            const bool isDirectMessages = category->type == QStringLiteral("direct_messages");
            if (isDirectMessages) {
                fallback = Candidate {teamItem, categoryItem, true};
                break;
            }
            if (!fallback.categoryItem) {
                fallback = Candidate {teamItem, categoryItem, false};
            }
        }

        if (fallback.directMessages) {
            break;
        }
    }

    if (!fallback.teamItem || !fallback.categoryItem) {
        qDebug() << "openStoredChannel" << channelID << ": no sidebar category contains channel";
        return;
    }

    ChannelItem* item = createChannelItem(*backendForSidebar,
                                          *fallback.teamItem,
                                          *fallback.categoryItem,
                                          *channel);
    if (!item) {
        return;
    }

    fallback.categoryItem->setExpanded(true);
    if (currentItem() == item) {
        activateChannelItem(item);
    } else {
        setCurrentItem(item);
    }
}

} // namespace Mattermost
