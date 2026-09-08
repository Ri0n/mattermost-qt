/**
 * Copyright 2021, 2022 Lyubomir Filipov
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
 * along with Mattermost-QT. if not, see https://www.gnu.org/licenses/.
 */

#include "PostPoll.h"
#include "ui_PostPoll.h"

#include <QMessageBox>
#include <QPushButton>
#include <QSizePolicy>

#include "backend/Backend.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPoll.h"
#include "chat-area/ChatArea.h"
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

} // namespace

PostPoll::PostPoll (Backend& backend, const BackendPost& post, BackendPoll& poll, QWidget *parent)
:QFrame(parent)
,ui(new Ui::PostPoll)
,backend (backend)
{
	ui->setupUi(this);

	ui->titleLabel->setText (poll.title);

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

	ui->textLabel->setText (text);

	for (const auto& option: poll.options) {
		QPushButton* pushButton = new QPushButton (option.name, this);
		pushButton->setMinimumHeight(30);
		pushButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		pushButton->setToolTip (option.voters);

		if (poll.hasEnded) {
			pushButton->setDisabled (true);
		}

		ui->verticalLayout->addWidget(pushButton);

		// Keep this vector aligned 1:1 with BackendPoll::options. Poll metadata
		// reports indices in that full option array, which also contains admin
		// actions. Storing only voting buttons here made those indices diverge and
		// caused QList::operator[] assertions on valid metadata.
		optionButtons.push_back(pushButton);

		const QString actionId = option.actionID;
		if (actionId.isEmpty() || actionId.startsWith("vote")) {
			connect(pushButton, &QPushButton::released, this,
			        [this, &post, actionId] {
				sendPollAction(this->backend, this, post, actionId);
			});
		} else {
			adminButtons.push_back(pushButton);
			connect(pushButton, &QPushButton::released, this,
			        [this, &post, actionId, pushButton] {
				if (QMessageBox::question(
				        this, tr("Are you sure?"),
				        tr("Are you sure that you want to %1?").arg(pushButton->text()))
				    == QMessageBox::Yes) {
					sendPollAction(this->backend, this, post, actionId);
				}
			});

			QPalette pal = pushButton->palette();
			pal.setColor (QPalette::Button, QColor(Qt::darkGray));
			pushButton->setPalette (pal);
			pushButton->setVisible (poll.metadata.hasAdminPermissions);
		}
	}

	// Use this PostPoll as the QObject connection context. The BackendPoll model
	// can outlive a materialized PostWidget, so a context-less lambda capturing
	// this would otherwise run after the poll widget had been evicted.
	connect(&poll, &BackendPoll::onMetadataUpdated, this, [this, &poll] {
		for (QPushButton* adminButton: adminButtons) {
			if (adminButton) {
				adminButton->setVisible(poll.metadata.hasAdminPermissions);
			}
		}

		for (QPushButton* button: optionButtons) {
			if (!button) {
				continue;
			}
			QFont font = button->font();
			font.setBold(false);
			button->setFont(font);
		}

		for (uint32_t idx: poll.metadata.ownVoteOptions) {
			if (idx >= static_cast<uint32_t>(optionButtons.size())) {
				continue;
			}
			QPushButton* button = optionButtons.at(static_cast<int>(idx));
			if (!button) {
				continue;
			}
			QFont font = button->font();
			font.setBold(true);
			button->setFont(font);
		}

		ui->verticalLayout->invalidate();
		updateGeometry();
		if (QWidget* parent = parentWidget()) {
			parent->updateGeometry();
		}
	});

	// Request metadata only after the buttons and the lifetime-safe callback are
	// in place. This also removes a needless race with very fast/local responses.
	if (!poll.id.isEmpty()) {
		backend.retrievePollMetadata (poll);
	}
}

PostPoll::~PostPoll()
{
    delete ui;
}

} /* namespace Mattermost */