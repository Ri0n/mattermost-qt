#include "ChannelTree.h"

#include <QPointer>

#include "ChannelItem.h"
#include "backend/Backend.h"
#include "backend/Storage.h"
#include "backend/UserProfileService.h"
#include "backend/types/BackendChannel.h"

namespace Mattermost {

void ChannelTree::ensureGroupChannelDisplayName(const QString& channelId)
{
    if (!backendForSidebar || channelId.isEmpty()) {
        return;
    }

    BackendChannel* channel = backendForSidebar->getStorage().getChannelById(channelId);
    if (!channel || channel->type != BackendChannel::groupChannel) {
        return;
    }

    QPointer<ChannelTree> treeGuard(this);
    QPointer<BackendChannel> channelGuard(channel);
    UserProfileService::instance(*backendForSidebar).ensureGroupChannelMembers(
        *channel,
        [treeGuard, channelGuard, channelId] {
            if (!treeGuard || !channelGuard) {
                return;
            }

            const auto rows = treeGuard->channelToItemMap.value(channelId);
            for (QTreeWidgetItem* row : rows) {
                if (!row
                    || row->data(0, SidebarItem::KindRole).toInt() != SidebarItem::Channel) {
                    continue;
                }
                static_cast<ChannelItem*>(row)->setLabel(channelGuard->display_name);
            }

            if (treeGuard->viewport()) {
                treeGuard->viewport()->update();
            }
        });
}

} // namespace Mattermost
