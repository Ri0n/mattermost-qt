/**
 * @file GroupTeamItem.cpp
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

#include "GroupTeamItem.h"

#include <QMenu>

#include "backend/Backend.h"
#include "backend/types/BackendChannel.h"
#include "channel-tree/ChannelTree.h"
#include "channel-tree/channel-item/GroupChannelItem.h"
#include "channel-tree-dialogs/TeamChannelsListDialog.h"
#include "channel-tree-dialogs/UserSearchDialog.h"
#include "channel-tree-dialogs/ViewTeamMembersListDialog.h"

namespace Mattermost {

ChannelItem* GroupTeamItem::createChannelItem (Backend& backendRef, ChannelItemWidget* itemWidget)
{
	return new GroupChannelItem (backendRef, itemWidget);
}

void GroupTeamItem::showContextMenu (const QPoint& pos)
{
	QMenu myMenu;

	myMenu.addAction ("View Team Members", [this] {
		BackendTeam* team = backend.getStorage().getTeamById(teamId);
		if (!team) {
			return;
		}

		ViewTeamMembersDialog* dialog = new ViewTeamMembersListDialog (backend, *team, treeWidget());
		dialog->show ();
	});

	myMenu.addAction ("Start direct message", [this] {
		QSet<QString> existingDirectUsers;
		for (auto it = backend.getStorage().channels.cbegin();
		     it != backend.getStorage().channels.cend(); ++it) {
			const BackendChannel* channel = it.value();
			if (channel && channel->type == BackendChannel::directChannel && !channel->name.isEmpty()) {
				existingDirectUsers.insert(channel->name);
			}
		}

		FilterListDialogConfig dialogCfg {
			"Start direct message - Mattermost",
			"Search for a user to message:",
			"Search users by name:",
			QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
			" already has a direct conversation"
		};

		auto* dialog = new UserSearchDialog(
			backend, dialogCfg, UserSearchOptions {}, existingDirectUsers, treeWidget());
		dialog->show();

		QObject::connect(dialog, &UserSearchDialog::accepted, [this, dialog] {
			const BackendUser* user = dialog->getSelectedUser();
			if (!user) {
				return;
			}

			if (const BackendChannel* existing = backend.getStorage().getDirectChannelByUserId(user->id)) {
				if (auto* tree = static_cast<ChannelTree*>(treeWidget())) {
					tree->openChannel(existing->id);
				}
				return;
			}

			backend.createDirectChannel(*user);
		});
	});

	myMenu.addAction ("Add user to the team", [this] {
		BackendTeam* team = backend.getStorage().getTeamById(teamId);
		if (!team) {
			return;
		}

		FilterListDialogConfig dialogCfg {
			"Add user to team - Mattermost",
			"Search for a user to add to the '" + team->display_name + "' team:",
			"Search users by name:",
			QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
			""
		};

		UserSearchOptions options;
		options.notInTeamId = team->id;
		auto* dialog = new UserSearchDialog(backend, dialogCfg, options, {}, treeWidget());
		dialog->show();

		QObject::connect(dialog, &UserSearchDialog::accepted, [this, team, dialog] {
			const BackendUser* user = dialog->getSelectedUser();
			if (!user) {
				return;
			}
			backend.addUserToTeam(*team, user->id);
		});
	});

	myMenu.addAction ("View Public Channels", [this] {
		BackendTeam* team = backend.getStorage().getTeamById(teamId);
		if (!team) {
			return;
		}

		backend.retrieveTeamPublicChannels (team->id, [this, team] (std::list<BackendChannel>& channels) {
			FilterListDialogConfig dialogCfg {
				"Public Channels - Mattermost",
				"Public Channels in team '" + team->display_name + "':",
				"Filter channels by name:",
				QDialogButtonBox::Close,
				""
			};

			TeamChannelsListDialog* dialog = new TeamChannelsListDialog (backend, dialogCfg, channels, treeWidget());
			dialog->show ();
		});
	});

	myMenu.exec (pos);
}

} /* namespace Mattermost */
