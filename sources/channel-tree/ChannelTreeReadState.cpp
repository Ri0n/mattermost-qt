#include "ChannelTree.h"

#include <QMouseEvent>

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

    SidebarService::instance(*backendForSidebar).markChannelViewedLocally(*channel);
    backendForSidebar->markChannelAsViewed(*channel);
}

void ChannelTree::currentChanged(const QModelIndex& current, const QModelIndex& previous)
{
    QTreeWidget::currentChanged(current, previous);

    // Only the visible Channels tab owns this acknowledgement. Alternate views
    // (Recent/Attention) have their own explicit selection paths, while sidebar
    // rebuilds are programmatic and must not accidentally consume unread state.
    if (renderingSidebar || !isVisible() || !current.isValid()) {
        return;
    }

    markChannelViewed(itemFromIndex(current));
}

void ChannelTree::mousePressEvent(QMouseEvent* event)
{
    QTreeWidgetItem* previousItem = currentItem();
    QTreeWidget::mousePressEvent(event);

    // currentChanged() handles normal navigation. A click on the already-current
    // row has no current-index transition, but it is still an explicit user
    // acknowledgement and should mark newly arrived unread messages as viewed.
    if (!renderingSidebar && isVisible() && previousItem && previousItem == currentItem()) {
        QTreeWidgetItem* clickedItem = itemAt(event->pos());
        if (clickedItem == previousItem) {
            markChannelViewed(clickedItem);
        }
    }
}

} // namespace Mattermost
