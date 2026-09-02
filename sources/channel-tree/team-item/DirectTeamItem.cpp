/**
 * @file DirectTeamItem.cpp
 * @brief
 * @author Lyubomir Filipov
 * @date Jun 21, 2022
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

#include "DirectTeamItem.h"

#include <QMenu>

#include "backend/Backend.h"
#include "backend/types/BackendChannel.h"
#include "channel-tree/ChannelTree.h"
#include "channel-tree/channel-item/DirectChannelItem.h"
#include "channel-tree-dialogs/UserSearchDialog.h"

namespace Mattermost {

ChannelItem* DirectTeamItem::createChannelItem (Backend& backendRef, ChannelItemWidget* itemWidget)
{
	return new DirectChannelItem (backendRef, itemWidget);
}

void DirectTeamItem::showContextMenu (const QPoint& pos)
{
	QMenu myMenu;

	myMenu.addAction ("Add direct channel", [this] {
		QSet<QString> existingDirectUsers;
		for (auto it = backend.getStorage().channels.cbegin();
		     it != backend.getStorage().channels.cend(); ++it) {
			const BackendChannel* channel = it.value();
			if (channel && channel->type == BackendChannel::directChannel && !channel->name.isEmpty()) {
				existingDirectUsers.insert(channel->name);
			}
		}

		FilterListDialogConfig dialogCfg {
			"Add direct channel - Mattermost",
			"Search for a user to start a direct channel with:",
			"Search users by name:",
			QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
			" is already added"
		};

		auto* dialog = new UserSearchDialog(
			backend, dialogCfg, UserSearchOptions {}, existingDirectUsers, treeWidget());
		dialog->show ();

		connect (dialog, &UserSearchDialog::accepted, [this, dialog] {
			const BackendUser* user = dialog->getSelectedUser();
			if (!user) {
				return;
			}

			const BackendChannel* existingChannel = backend.getStorage().getDirectChannelByUserId(user->id);
			if (existingChannel) {
				ChannelTree* tree = static_cast<ChannelTree*> (this->treeWidget());
				tree->openChannel (existingChannel->id);
			} else {
				backend.createDirectChannel (*user);
			}
		});
	});

	myMenu.exec (pos);
}

} /* namespace Mattermost */
