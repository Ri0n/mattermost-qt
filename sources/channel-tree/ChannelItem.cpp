/**
 * @file ChannelItem.cpp
 * @brief
 * @author Lyubomir Filipov
 * @date Mar 30, 2022
 *
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Mattermost-QT is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Mattermost-QT. if not, see https://www.gnu.org/licenses/.
 */

#include "ChannelItem.h"

#include <QMenu>

#include "ChannelItemWidget.h"
#include "ChannelTree.h"
#include "backend/SidebarService.h"
#include "backend/types/BackendChannel.h"

namespace Mattermost {

ChannelItem::ChannelItem (Backend& backend, ChannelItemWidget* widget)
:ChannelTreeItem ()
,backend (backend)
,widget (widget)
{
	QFont font1;
	font1.setBold (true);
	font1.setPixelSize(14);
	setFont (1, font1);
}

ChannelItem::~ChannelItem () = default;

void ChannelItem::setIcon (const QIcon& icon)
{
	if (widget) {
		widget->setIcon (icon);
	}
}

void ChannelItem::setLabel (const QString& label)
{
	if (widget) {
		widget->setLabel (label);
	}
}

void ChannelItem::setWidget (ChannelItemWidget* widget)
{
	this->widget = widget;
}

void ChannelItem::setMuted(bool muted)
{
    if (widget) {
        widget->setMuted(muted);
    }
}

void ChannelItem::addCommonContextMenuActions(QMenu& menu, BackendChannel& channel)
{
    auto& sidebar = SidebarService::instance(backend);
    const bool muted = sidebar.isChannelMuted(channel);
    const bool conversation = channel.type == BackendChannel::directChannel
        || channel.type == BackendChannel::groupChannel;

    menu.addAction(muted
                       ? (conversation ? QStringLiteral("Unmute Conversation") : QStringLiteral("Unmute Channel"))
                       : (conversation ? QStringLiteral("Mute Conversation") : QStringLiteral("Mute Channel")),
                   [this, &channel, muted] {
        SidebarService::instance(backend).setChannelMuted(channel, !muted);
    });

    auto* tree = static_cast<ChannelTree*>(treeWidget());
    if (tree && tree->canRemoveChannelFromCategory(this)) {
        menu.addAction(QStringLiteral("Remove from group"), [tree, this] {
            tree->removeChannelFromCategory(this);
        });
    }
}

} /* namespace Mattermost */
