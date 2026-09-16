/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "ChannelTree.h"

#include <QTimer>

#include "backend/Backend.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "channel-tree/ChannelItem.h"

namespace Mattermost {

void ChannelTree::rowsInserted(const QModelIndex& parent, int start, int end)
{
    QTreeWidget::rowsInserted(parent, start, end);

    // QTreeWidgetItem::addChild() notifies the view before ChannelTree has
    // finished assigning the item's semantic roles and registering it in
    // channelToItemMap. Defer one event-loop turn and coalesce the startup burst
    // so the completed rows can be wired to their BackendChannel objects.
    scheduleChannelDisplaySync();
}

void ChannelTree::scheduleChannelDisplaySync()
{
    if (channelDisplaySyncScheduled) {
        return;
    }

    channelDisplaySyncScheduled = true;
    QTimer::singleShot(0, this, [this] {
        channelDisplaySyncScheduled = false;
        syncChannelDisplayRows();
    });
}

void ChannelTree::syncChannelDisplayRows()
{
    if (!backendForSidebar) {
        return;
    }

    for (auto it = channelToItemMap.cbegin(); it != channelToItemMap.cend(); ++it) {
        BackendChannel* channel = backendForSidebar->getStorage().getChannelById(it.key());
        if (!channel) {
            continue;
        }

        // A channel may have several sidebar rows and those rows are routinely
        // destroyed/recreated when categories refresh. Keep exactly one
        // channel-level connection and resolve the currently alive rows from
        // channelToItemMap when the signal arrives.
        connect(channel, &BackendChannel::onUpdated,
                this, &ChannelTree::handleChannelUpdated,
                Qt::UniqueConnection);

        // The profile update may have raced ahead of the deferred connection.
        // Synchronize immediately as well so a freshly materialized DM never
        // remains stuck on the raw server channel id/name.
        for (QTreeWidgetItem* row : it.value()) {
            if (!row || row->data(0, ItemKindRole).toInt() != ChannelItemKind) {
                continue;
            }
            static_cast<ChannelItem*>(row)->setLabel(channel->display_name);
        }
    }
}

void ChannelTree::handleChannelUpdated()
{
    auto* channel = qobject_cast<BackendChannel*>(sender());
    if (!channel) {
        return;
    }

    const auto items = channelToItemMap.value(channel->id);
    for (QTreeWidgetItem* row : items) {
        if (!row || row->data(0, ItemKindRole).toInt() != ChannelItemKind) {
            continue;
        }
        static_cast<ChannelItem*>(row)->setLabel(channel->display_name);
    }
}

} // namespace Mattermost
