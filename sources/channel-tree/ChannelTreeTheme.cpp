#include "ChannelTree.h"

#include <QEvent>

#include "ChannelIcons.h"
#include "ChannelItem.h"
#include "backend/types/BackendChannel.h"

namespace Mattermost {

void ChannelTree::changeEvent(QEvent* event)
{
    QTreeWidget::changeEvent(event);
    if (!event || (event->type() != QEvent::PaletteChange
                   && event->type() != QEvent::ApplicationPaletteChange
                   && event->type() != QEvent::StyleChange)) {
        return;
    }

    viewport()->setPalette(palette());
    refreshPaletteDependentIcons();
    viewport()->update();
}

void ChannelTree::refreshPaletteDependentIcons()
{
    for (auto mapIt = channelToItemMap.cbegin(); mapIt != channelToItemMap.cend(); ++mapIt) {
        for (QTreeWidgetItem* row : mapIt.value()) {
            if (!row || row->data(0, ItemKindRole).toInt() != ChannelItemKind) {
                continue;
            }

            auto* channelItem = static_cast<ChannelItem*>(row);
            const int type = row->data(0, ItemChannelTypeRole).toInt();
            if (type == BackendChannel::groupChannel) {
                channelItem->setIcon(ChannelIcons::groupConversation());
            } else if (type == BackendChannel::publicChannel
                       || type == BackendChannel::privateChannel) {
                channelItem->setIcon(ChannelIcons::channel());
            }
        }
    }
}

} // namespace Mattermost
