#include "ChannelTree.h"

#include <QMouseEvent>
#include <QTimer>

#include "backend/Backend.h"
#include "backend/SidebarService.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"

namespace Mattermost {

void ChannelTree::markChannelViewed(QTreeWidgetItem* item)
{
    if (!backendForSidebar || !item
        || item->data(0, ItemKindRole).toInt() != ChannelItemKind) {
        return;
    }

    const QString channelId = item->data(0, ItemIdRole).toString();
    BackendChannel* channel = backendForSidebar->getStorage().getChannelById(channelId);
    if (!channel) {
        return;
    }

    auto& sidebar = SidebarService::instance(*backendForSidebar);
    if (!sidebar.isChannelUnread(*channel) && !sidebar.hasUnreadMention(channelId)) {
        return;
    }

    sidebar.markChannelViewedLocally(*channel);
    backendForSidebar->markChannelAsViewed(*channel);
}

void ChannelTree::currentChanged(const QModelIndex& current, const QModelIndex& previous)
{
    QTreeWidget::currentChanged(current, previous);

    // Sidebar rebuilds are programmatic and must never consume unread state.
    // Every other channel transition is an explicit navigation path: Channels,
    // Recent, Attention and notification activation all end up here.
    if (renderingSidebar || !current.isValid()) {
        return;
    }

    QTreeWidgetItem* item = itemFromIndex(current);
    if (!item || item->data(0, ItemKindRole).toInt() != ChannelItemKind) {
        return;
    }

    const QString channelId = item->data(0, ItemIdRole).toString();
    if (channelId.isEmpty()) {
        return;
    }

    // Recent and Attention also acknowledge their own selection synchronously.
    // Defer the shared ChannelTree acknowledgement by one event-loop turn and
    // re-check unread state so those paths never issue duplicate server calls.
    QTimer::singleShot(0, this, [this, channelId] {
        if (!backendForSidebar) {
            return;
        }
        BackendChannel* channel = backendForSidebar->getStorage().getChannelById(channelId);
        if (!channel) {
            return;
        }
        auto& sidebar = SidebarService::instance(*backendForSidebar);
        if (!sidebar.isChannelUnread(*channel) && !sidebar.hasUnreadMention(channelId)) {
            return;
        }
        sidebar.markChannelViewedLocally(*channel);
        backendForSidebar->markChannelAsViewed(*channel);
    });
}

void ChannelTree::mousePressEvent(QMouseEvent* event)
{
    QTreeWidgetItem* previousItem = currentItem();
    QTreeWidget::mousePressEvent(event);

    // currentChanged() handles normal navigation. A click on the already-current
    // row has no current-index transition, but it is still an explicit user
    // acknowledgement and should mark newly arrived unread messages as viewed.
    if (!renderingSidebar && previousItem && previousItem == currentItem()) {
        QTreeWidgetItem* clickedItem = itemAt(event->pos());
        if (clickedItem == previousItem) {
            markChannelViewed(clickedItem);
        }
    }
}

} // namespace Mattermost
