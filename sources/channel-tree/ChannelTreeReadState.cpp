#include "ChannelTree.h"

#include <QMouseEvent>
#include <QPointer>
#include <QTimer>

#include "ChannelItem.h"
#include "backend/Backend.h"
#include "backend/PostTimelineService.h"
#include "backend/SidebarService.h"
#include "backend/types/BackendChannel.h"
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
    // Direct-message recency sorting moves existing QTreeWidgetItems with
    // takeChild()/insertChild(). Taking the current row temporarily detaches it
    // from the tree and Qt selects a neighbouring row. Treating that transient
    // model mutation as navigation activates an unrelated ChatArea even though
    // the user never selected it. Keep the visible conversation authoritative
    // until its row has been reinserted, then restore the tree current item.
    ChatArea* activePage = getCurrentPage();
    if (!renderingSidebar && activePage && activePage->treeItem
        && activePage->treeItem->treeWidget() != this) {
        QPointer<ChatArea> activeGuard(activePage);
        QTimer::singleShot(0, this, [this, activeGuard] {
            if (!activeGuard || !activeGuard->treeItem
                || activeGuard->treeItem->treeWidget() != this) {
                return;
            }
            setCurrentItem(activeGuard->treeItem);
        });
        return;
    }

    // Capture the first-open unread boundary before QTreeWidget::currentChanged
    // emits currentItemChanged. That signal synchronously activates ChatArea and
    // schedules read acknowledgement; the initial unread request must retain the
    // membership state from the user's actual selection gesture.
    QString initialUnreadChannelId;
    uint64_t initialLastViewedAt = 0;
    if (!renderingSidebar && backendForSidebar && current.isValid()) {
        QTreeWidgetItem* incomingItem = itemFromIndex(current);
        if (incomingItem
            && incomingItem->data(0, ItemKindRole).toInt() == ChannelItemKind
            && !incomingItem->data(0, Qt::UserRole).value<ChatArea*>()) {
            const QString incomingChannelId = incomingItem->data(0, ItemIdRole).toString();
            BackendChannel* incomingChannel = backendForSidebar->getStorage().getChannelById(
                incomingChannelId);
            auto& sidebar = SidebarService::instance(*backendForSidebar);
            if (incomingChannel && sidebar.isChannelUnread(*incomingChannel)) {
                initialUnreadChannelId = incomingChannelId;
                initialLastViewedAt = sidebar.channelLastViewedTime(incomingChannelId);
            }
        }
    }

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

    if (!initialUnreadChannelId.isEmpty()
        && initialUnreadChannelId == channelId
        && backendForSidebar) {
        BackendChannel* channel = backendForSidebar->getStorage().getChannelById(channelId);
        if (channel) {
            QPointer<ChannelTree> guard(this);
            PostTimelineService::instance(*backendForSidebar).loadChannelUnread(
                *channel, 30, 30, initialLastViewedAt,
                [guard, channelId](const PostTimelineService::Page& page) {
                    if (!guard || !page.success || page.firstUnreadPostId.isEmpty()) {
                        return;
                    }

                    ChatArea* currentPage = guard->getCurrentPage();
                    if (!currentPage || currentPage->getChannel().id != channelId) {
                        return;
                    }

                    // Make the server-selected unread context authoritative for
                    // this first opening. The zero quiet period keeps the target
                    // stable through asynchronous layout until the user scrolls.
                    currentPage->lockNavigationToPost(page.firstUnreadPostId, 0);
                    currentPage->ensurePinnedPostVisible(
                        page.firstUnreadPostId,
                        page.postIds,
                        page.prevPostId.isEmpty(),
                        page.nextPostId.isEmpty());
                });
        }
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
    // row has no current-index transition, but is still an explicit navigation
    // and read intent. Re-activate it as well so a stale stacked-page mismatch
    // can always be repaired by clicking the selected conversation again.
    if (!renderingSidebar && previousItem && previousItem == currentItem()) {
        QTreeWidgetItem* clickedItem = itemAt(event->pos());
        if (clickedItem == previousItem
            && clickedItem->data(0, ItemKindRole).toInt() == ChannelItemKind) {
            activateChannelItem(clickedItem);
            markChannelViewed(clickedItem);
        }
    }
}

} // namespace Mattermost
