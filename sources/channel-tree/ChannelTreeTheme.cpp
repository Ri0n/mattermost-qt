/**
 * Copyright 2026 Sergei Ilinykh
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
 * along with Mattermost-QT. If not, see https://www.gnu.org/licenses/.
 */

#include "ChannelTree.h"

#include <QEvent>
#include <QPalette>

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

    // The viewport must keep inheriting the tree/application palette. Copying
    // palette() here resolves every role explicitly and freezes the current
    // light/dark colours for the next desktop-theme change.
    viewport()->setPalette(QPalette());
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
