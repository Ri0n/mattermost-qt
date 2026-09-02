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

#include "PostWidget.h"

#include <iostream>
#include <QDebug>
#include <QDateTime>
#include <QPointer>
#include <QResizeEvent>
#include <QPushButton>
#include "backend/Backend.h"
#include "backend/UserProfileService.h"
#include "backend/types/BackendPost.h"
#include "backend/emoji/EmojiInfo.h"
#include "chat-area/ChatArea.h"
#include "MessageContentWidget.h"
#include "MessageFormatter.h"
#include "PostQuoteFrame.h"
#include "attachments/PostAttachmentList.h"
#include "attachments/PostPoll.h"
#include "reactions/PostReactionList.h"
#include "ui_PostWidget.h"

namespace Mattermost {

PostWidget::PostWidget (Backend& backend, BackendPost &post, QWidget *parent, ChatArea* chatArea, BackendPost* lastRootPost)
:QWidget(parent)
,post (post)
,threadButton(nullptr)
,ui(new Ui::PostWidget)
,messageContent(nullptr)
,parentChatArea(chatArea)
{
	ui->setupUi(this);
	ui->authorName->setText (post.getDisplayAuthorName ());

	if (post.isOwnPost()) {
		ui->authorName->setStyleSheet("QLabel { color : blue; }");
	}

	messageContent = new MessageContentWidget(this);
	const int messageIndex = ui->verticalLayout->indexOf(ui->message);
	ui->verticalLayout->removeWidget(ui->message);
	ui->message->hide();
	ui->verticalLayout->insertWidget(messageIndex, messageContent);
	connect(messageContent, &MessageContentWidget::dimensionsChanged,
			this, &PostWidget::dimensionsChanged);
	messageContent->setMessage(post.message);
	ui->time->setText (getMessageTimeString (post.create_at));

	connect(messageContent, &MessageContentWidget::linkHovered, this, [this](const QString& link) {
		qDebug() << "Link hovered: " << link;
		hoveredLink = link;
	});

	if (post.author) {
		setAuthor(backend, post.author);
	} else if (!post.user_id.isEmpty()) {
		QPointer<PostWidget> guard(this);
		UserProfileService::instance(backend).ensureUser(
			post.user_id, [guard, &backend](const BackendUser* user) {
				if (guard && user) {
					guard->setAuthor(backend, user);
				}
			});
	}

	/**
	 * Add root post as a quote box.
	 * Multiple consecutive posts, quoting the same post will have the quote added only to the first of them.
	 */
	if (post.rootPost && post.rootPost != lastRootPost) {

		quoteFrame = std::make_unique<PostQuoteFrame> (*post.rootPost, backend.getStorage(), this);

		//insert the frame after the post author line
		ui->verticalLayout->insertWidget (1, quoteFrame.get(), 0, Qt::AlignLeft);

		connect (quoteFrame.get(), &PostQuoteFrame::postClicked, [&post, chatArea] {
			chatArea->goToPost (*post.rootPost);
		});
	}

	//Add previews for files, if any
	if (!post.files.empty()) {
		attachments = std::make_unique<PostAttachmentList> (backend, this);
		connect(attachments.get(), &PostAttachmentList::dimensionsChanged,
				this, &PostWidget::dimensionsChanged);
		ui->verticalLayout->addWidget (attachments.get(), 0, Qt::AlignLeft);
		for (const BackendFile& file: post.files) {
			attachments->addFile (file, post.getDisplayAuthorName());
		}
	}

	//Add reactions, if any
	if (!post.reactions.empty()) {
		reactions = std::make_unique<PostReactionList> (this);

		for (auto& it: post.reactions) {
			EmojiID emojiID = it.first;
			Emoji emoji = EmojiInfo::getEmoji (emojiID);
			reactions->addReaction (emoji.name, emoji.unicodeString, it.second);
		}

		ui->verticalLayout->addWidget (reactions.get(), 0, Qt::AlignLeft);
	}

	if (post.poll) {
		//clear message text, because poll messages do not contain free text (outside the poll itself)
		clearMessageText ();
		poll = std::make_unique<PostPoll> (backend, post, *post.poll, this);
		ui->verticalLayout->addWidget (poll.get(), 0, Qt::AlignLeft);
	}

	if (post.has_thread && !parentChatArea->isThread) {
		threadButton = new QPushButton("Open Thread", this);
		connect (threadButton, &QPushButton::clicked, this, &PostWidget::openThreadWindow);
		ui->verticalLayout->addWidget(threadButton);
	}
}

PostWidget::~PostWidget()
{
    delete ui;
}

void PostWidget::setAuthor(Backend& backend, const BackendUser* user)
{
	if (!user) {
		return;
	}

	post.author = user;
	ui->authorName->setText(post.getDisplayAuthorName());
	if (post.isOwnPost()) {
		ui->authorName->setStyleSheet("QLabel { color : blue; }");
	}

	connect(user, &BackendUser::onAvatarChanged, this, [this, user] {
		if (post.author == user) {
			ui->authorAvatar->setPixmap(user->avatar);
		}
	}, Qt::UniqueConnection);

	if (user->avatar.isNull()) {
		backend.retrieveUserAvatar(user->id);
	} else {
		ui->authorAvatar->setPixmap(user->avatar);
	}
}

void PostWidget::setEdited (const QString& message)
{
	messageContent->setMessage(message);

	/**
	 * if (there is a poll in the post, just recreate the poll instance
	 */
	if (post.poll) {
		//clear message text, because poll messages do not contain free text (outside the poll itself)
		clearMessageText ();
		std::unique_ptr<PostPoll> newPoll = std::make_unique<PostPoll> (poll->backend, post, *post.poll, this);
		ui->verticalLayout->replaceWidget (poll.get(), newPoll.get());
		poll = std::move (newPoll);
	}
}

void PostWidget::updateReactions ()
{
	if (reactions) {
		reactions.reset ();
	}

	//Add reactions, if any
	if (!post.reactions.empty()) {
		reactions = std::make_unique<PostReactionList> (this);

		for (auto& it: post.reactions) {
			EmojiID emojiID = it.first;
			Emoji emoji = EmojiInfo::getEmoji (emojiID);
			reactions->addReaction (emoji.name, emoji.unicodeString, it.second);
		}

		ui->verticalLayout->addWidget (reactions.get(), 0, Qt::AlignLeft);
	}
}

void PostWidget::addThreadButton ()
{
	if (threadButton == nullptr)
		threadButton = new QPushButton("Open Thread", this);
	connect (threadButton, SIGNAL(clicked()), this, SLOT(openThreadWindow()));
	ui->verticalLayout->addWidget(threadButton);
}

void PostWidget::openThreadWindow () {
	ChatArea * area;
	if (parentChatArea->threadsAreas.empty()){
		area = new ChatArea(parentChatArea->backend, parentChatArea->channel, post.id, parentChatArea);
		area->root_id = post.id;
		parentChatArea->threadsAreas.insert(area);
		area->show();	
	} else {
		auto it = parentChatArea->threadsAreas.begin(), end = parentChatArea->threadsAreas.end();
		for (; it != end; ++it){
			if ((*it)->root_id == post.id) {
				(*it)->activateWindow();
				qDebug() << "exists";
				break;
			}
		}
		if (it == parentChatArea->threadsAreas.end()){
			area = new ChatArea(parentChatArea->backend, parentChatArea->channel, post.id, parentChatArea);
			area->root_id = post.id;
			parentChatArea->threadsAreas.insert(area);
			area->show();
		}
	}

	qDebug() << post.id << post.has_thread << post.hidden << post.root_id;
}

void PostWidget::markAsDeleted ()
{
	attachments.reset (nullptr);
	post.isDeleted = true;
	if (poll) {
		ui->verticalLayout->removeWidget (poll.get());
		poll.reset (nullptr);

		messageContent->setMessage(QStringLiteral("(Poll deleted)"));
	} else {
		messageContent->setMessage(QStringLiteral("(Message deleted)"));
	}
}


QString PostWidget::getSelectedText ()
{
	return messageContent->selectedText();
}

QString PostWidget::formatMessageText (const QString& str)
{
	return MessageFormatter::formatMessageText(str);
}

QString PostWidget::getMessageTimeString (uint64_t timestamp)
{
	QDate currentDate = QDateTime::currentDateTime().date();
	QDateTime postTime = QDateTime::fromMSecsSinceEpoch (timestamp);
	QDate postDate = postTime.date();

	QString format;

	if (currentDate.year() != postDate.year()) {
		format = "dd MMM yyyy, hh:mm:ss";
	} else if (currentDate.day() != postDate.day() || currentDate.month() != postDate.month()) {
		format = "dd MMM, hh:mm:ss";
	} else {
		format = "hh:mm:ss";
	}

	return postTime.toString (format);
}

QString PostWidget::formatForClipboardSelection (FormatType formatType) const
{
	if (formatType == messageOnly) {
		return post.message;
	}

	QString ret (post.getDisplayAuthorName() + "\t[" + ui->time->text() + "]\n");
	ret += " " + post.message + "\n\n";
	return ret;
}

void PostWidget::clearMessageText ()
{
	messageContent->clear();
}

} /* namespace Mattermost */
