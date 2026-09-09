/**
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

#include "PostPoll.h"
#include "ui_PostPoll.h"

#include <QPushButton>
#include <QSizePolicy>

#include "backend/Backend.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPoll.h"
#include "backend/types/BackendPost.h"
#include "chat-area/ChatArea.h"
#include "channel-tree/ChannelItem.h"
#include "channel-tree/SidebarItem.h"

namespace Mattermost {
namespace {

QString pollActionTeamContextId(QWidget* origin, Backend& backend, const BackendPost& post)
{
	if (BackendChannel* actionChannel = backend.getStorage().getChannelById(post.channel_id)) {
		if (actionChannel->team) {
			return actionChannel->team->id;
		}
	}

	for (QWidget* widget = origin; widget; widget = widget->parentWidget()) {
		auto* area = qobject_cast<ChatArea*>(widget);
		if (!area) {
			continue;
		}

		for (ChatArea* contextArea = area; contextArea;
		     contextArea = contextArea->parentChatArea()) {
			if (contextArea->channel.team) {
				return contextArea->channel.team->id;
			}
			if (!contextArea->treeItem) {
				continue;
			}

			const QString teamId = contextArea->treeItem
				->data(0, SidebarItem::TeamIdRole).toString();
			if (!teamId.isEmpty()) {
				return teamId;
			}
		}
		break;
	}

	return backend.getCurrentTeamContextId();
}

void sendPollAction(Backend& backend, QWidget* origin,
                    const BackendPost& post, const QString& actionId)
{
	const QString teamId = pollActionTeamContextId(origin, backend, post);
	if (!teamId.isEmpty()) {
		backend.setCurrentTeamContextId(teamId);
	}
	backend.sendPostAction(post, actionId);
}

bool isAddOptionAction(const QString& actionId)
{
	return actionId == QStringLiteral("addOption");
}

bool isPollManagementAction(const QString& actionId)
{
	return actionId == QStringLiteral("endPoll")
		|| actionId == QStringLiteral("deletePoll");
}

} // namespace

PostPoll::PostPoll (Backend& backend, const BackendPost& post, BackendPoll& poll, QWidget *parent)
:QFrame(parent)
,ui_(new Ui::PostPoll)
,backend_(backend)
{
	ui_->setupUi(this);

	ui_->titleLabel->setText (poll.title);

	QString text (poll.text.toHtmlEscaped ());
	text.replace("---\n", "");
	text.replace("\n", "<br>");

	//replace stars with bold tags
	QString replacements[2] = { "<b>", "</b>" };
	int replacementIdx = 0;

	int start = 0;
	int pos = text.indexOf("**", start);

	while (pos != -1) {
		text.replace (pos, 2, replacements[(replacementIdx++) % 2]);
		start = pos + 2;
		pos = text.indexOf("**", start);
	}

	ui_->textLabel->setText (text);

	for (const auto& option: poll.options) {
		QPushButton* pushButton = new QPushButton (option.name, this);
		pushButton->setMinimumHeight(30);
		pushButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		pushButton->setToolTip (option.voters);

		if (poll.hasEnded) {
			pushButton->setDisabled (true);
		}

		ui_->verticalLayout->addWidget(pushButton);

		// Keep this vector aligned 1:1 with BackendPoll::options. Poll metadata
		// reports indices in that full option array, which also contains admin
		// actions. Storing only voting buttons here made those indices diverge and
		// caused QList::operator[] assertions on valid metadata.
		optionButtons_.push_back(pushButton);

		const QString actionId = option.actionID;
		connect(pushButton, &QPushButton::released, this,
		        [this, &post, actionId] {
			sendPollAction(backend_, this, post, actionId);
		});

		if (!actionId.isEmpty() && !actionId.startsWith(QStringLiteral("vote"))) {
			QPalette pal = pushButton->palette();
			pal.setColor (QPalette::Button, QColor(Qt::darkGray));
			pushButton->setPalette (pal);
		}

		if (isAddOptionAction(actionId)) {
			addOptionButtons_.push_back(pushButton);
			pushButton->setVisible(
				poll.metadata.hasAdminPermissions || poll.metadata.allowsPublicAddOption);
		} else if (isPollManagementAction(actionId)) {
			managementButtons_.push_back(pushButton);
			pushButton->setVisible(poll.metadata.hasAdminPermissions);
		}
	}

	// Use this PostPoll as the QObject connection context. The BackendPoll model
	// can outlive a materialized PostWidget, so a context-less lambda capturing
	// this would otherwise run after the poll widget had been evicted.
	connect(&poll, &BackendPoll::onMetadataUpdated, this, [this, &poll] {
		for (QPushButton* addOptionButton: addOptionButtons_) {
			if (addOptionButton) {
				addOptionButton->setVisible(
					poll.metadata.hasAdminPermissions || poll.metadata.allowsPublicAddOption);
			}
		}

		for (QPushButton* managementButton: managementButtons_) {
			if (managementButton) {
				managementButton->setVisible(poll.metadata.hasAdminPermissions);
			}
		}

		for (QPushButton* button: optionButtons_) {
			if (!button) {
				continue;
			}
			QFont font = button->font();
			font.setBold(false);
			button->setFont(font);
		}

		for (uint32_t idx: poll.metadata.ownVoteOptions) {
			if (idx >= static_cast<uint32_t>(optionButtons_.size())) {
				continue;
			}
			QPushButton* button = optionButtons_.at(static_cast<int>(idx));
			if (!button) {
				continue;
			}
			QFont font = button->font();
			font.setBold(true);
			button->setFont(font);
		}

		ui_->verticalLayout->invalidate();
		updateGeometry();
		if (QWidget* parent = parentWidget()) {
			parent->updateGeometry();
		}
	});

	// Request metadata only after the buttons and the lifetime-safe callback are
	// in place. This also removes a needless race with very fast/local responses.
	if (!poll.id.isEmpty()) {
		backend_.retrievePollMetadata (poll);
	}
}

PostPoll::~PostPoll()
{
    delete ui_;
}

} /* namespace Mattermost */
