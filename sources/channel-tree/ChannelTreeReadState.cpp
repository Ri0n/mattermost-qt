#include "ChannelTree.h"

#include <QMouseEvent>
#include <QTimer>

#include "chat-area/ChatArea.h"

namespace Mattermost {

void ChannelTree::markChannelViewed(QTreeWidgetItem* item)
{
    if (!item || item->data(0, ItemKindRole).toInt() != ChannelItemKind) {
        return;
    }

    const QString channelId = item->data(0, ItemIdRole).toString();
    ChatArea* page = getCurrentPage();
    if (!page || page->getChannel().id != channelId) {
        return;
    }

    // Selection expresses read intent. ChatArea waits until the newest channel
    // content is actually present in the model and rendered before committing
    // the local/server viewed state.
    page->requestExplicitReadAcknowledgement();
}

void ChannelTree::currentChanged(const QModelIndex& current, const QModelIndex& previous)
{
    QTreeWidget::currentChanged(current, previous);

    // Sidebar rebuilds are programmatic and must never consume unread state.
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

    // currentItemChanged activates/materializes the ChatArea. Defer the read
    // request until that synchronous navigation path has completed, and resolve
    // the item again so category/sidebar rebuilds cannot leave a stale pointer.
    QTimer::singleShot(0, this, [this, channelId] {
        QTreeWidgetItem* currentItemPtr = currentItem();
        if (!currentItemPtr
            || currentItemPtr->data(0, ItemKindRole).toInt() != ChannelItemKind
            || currentItemPtr->data(0, ItemIdRole).toString() != channelId) {
            return;
        }
        markChannelViewed(currentItemPtr);
    });
}

void ChannelTree::mousePressEvent(QMouseEvent* event)
{
    QTreeWidgetItem* previousItem = currentItem();
    QTreeWidget::mousePressEvent(event);

    // currentChanged() handles normal navigation. A click on the already-current
    // row has no current-index transition, but is still an explicit read intent.
    if (!renderingSidebar && previousItem && previousItem == currentItem()) {
        QTreeWidgetItem* clickedItem = itemAt(event->pos());
        if (clickedItem == previousItem) {
            markChannelViewed(clickedItem);
        }
    }
}

} // namespace Mattermost
