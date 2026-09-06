/**
 * @file OutgoingPostCreator.h
 * @brief
 * @author Lyubomir Filipov
 * @date Feb 20, 2022
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

#pragma once

#include <memory>
#include <QBoxLayout>

#include "MessageTextEditWidget.h"
#include "fwd.h"

class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QLabel;
class QPushButton;

namespace Mattermost {

struct OutgoingPostData;
class ChatLogWidget;

class OutgoingPostCreator: public MessageTextEditWidget {
	Q_OBJECT
public:
	explicit OutgoingPostCreator (QWidget *parent = nullptr);
	~OutgoingPostCreator();
public:
	void init(Backend& backend,
	          BackendChannel& channel,
	          ChatLogWidget& chatLogWidget,
	          QBoxLayout* attachmentParent,
	          QLabel& statusLabel,
	          QPushButton& attachButton,
	          QPushButton& addEmojiButton,
	          QPushButton& sendButton);
	void setRootId(QString id);
	void onDragEnterEvent (QDragEnterEvent* event);
	void onDragMoveEvent (QDragMoveEvent* event);
	void onDropEvent (QDropEvent* event);
	void setStatusLabelText (const QString& string);
	const BackendPost* editingPost() const { return postToEdit; }

public slots:
	void onAttachButtonClick ();
	void onPostReceived (BackendPost& post);
	void sendPostButtonAction ();
	void postEditInitiated (BackendPost& post);
	void cancelPostEdit ()
	{
		// Once an edit has been submitted, keep the immutable request data until
		// the HTTP transaction succeeds or the user explicitly retries it.
		if (!postToEdit || outgoingPostData) {
			return;
		}
		clear();
		postToEdit = nullptr;
		setEditingVisual(false);
		emit postEditFinished();
	}

signals:
	void postEditFinished ();

private:
	void createAttachmentList (QStringList& files);
	void updateSendButtonState ();
	void setEditingVisual(bool editing);
	void setSendActivityText();
	void finishSend(const QString& confirmedPostId = QString());
	void failSend();
	bool isEditingPost() const;
	bool isCreatingPost ();
	bool isWaitingForPostServerResponse ();

	void startSendPostSequence ();
	void prepareAndSendPost ();
	void sendPost ();

private:
	Backend*							backend = nullptr;
	BackendChannel*						channel = nullptr;
	ChatLogWidget*						chatLogWidget = nullptr;
	QLabel*								statusLabel = nullptr;
	QPushButton*						attachButton = nullptr;
	QPushButton*						addEmojiButton = nullptr;
	QPushButton*						sendButton = nullptr;
	const BackendPost*					postToEdit;
	OutgoingAttachmentList*				attachmentList;
	std::unique_ptr<OutgoingPostData> 	outgoingPostData;
	bool								sendFailed = false;
	QBoxLayout* 						attachmentParent = nullptr;
	QString						root_id;
};

} /* namespace Mattermost */
