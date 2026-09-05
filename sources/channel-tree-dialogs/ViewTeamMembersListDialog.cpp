/**
 * @file ViewTeamMembersListDialog.cpp
 * @brief 'View Team Members' context menu item dialog
 * @author Lyubomir Filipov
 * @date Apr 17, 2023
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

#include "ViewTeamMembersListDialog.h"

#include <QMenu>
#include <QPointer>

#include "backend/Backend.h"
#include "backend/UserProfileService.h"
#include "backend/types/BackendTeam.h"
#include "ui_FilterListDialog.h"

namespace Mattermost {

ViewTeamMembersListDialog::ViewTeamMembersListDialog (Backend& backend, BackendTeam& team, QWidget* parent)
:UserListDialog (parent)
,team (team)
,backend (backend)
{
    setProfileBackend(&backend);
	refreshMembers();

	QPointer<ViewTeamMembersListDialog> guard(this);
	UserProfileService::instance(backend).ensureTeamMembers(team, [guard] {
		if (guard) {
			guard->refreshMembers();
		}
	});

	connect (&team, &BackendTeam::onUserRemoved, this, &UserListDialog::removeRowByData);
}

ViewTeamMembersListDialog::~ViewTeamMembersListDialog () = default;

void ViewTeamMembersListDialog::refreshMembers()
{
	FilterListDialogConfig dialogCfg {
		"Team Members - Mattermost",
		"Members of team '" + team.display_name + "':",
		"Filter users by name:",
		QDialogButtonBox::Close,
		""
	};

	std::set<UserListEntry> entrySet;
	for (auto& it: team.members) {
		if (it.user) {
			entrySet.emplace (it);
		}
	}

	dataToItemMap.clear();
	ui->tableWidget->clearContents();
	create (dialogCfg, entrySet, {"Full Name", "Status"});
}

void ViewTeamMembersListDialog::addContextMenuActions (QMenu& menu, const QVariant& selectedItemData)
{
	BackendUser *user = selectedItemData.value<BackendUser*>();

	if (!user) {
		qDebug() << "No user at pointed item";
		return;
	}

	UserListDialog::addContextMenuActions (menu, selectedItemData);

	menu.addAction ("Remove from team", [this, user] {
		backend.removeUserFromTeam (team, user->id);
	});
}

} /* namespace Mattermost */
