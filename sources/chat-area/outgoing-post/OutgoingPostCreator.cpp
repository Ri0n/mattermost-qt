/**
 * @file OutgoingPostCreator.cpp
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

#include "OutgoingPostCreator.h"

#include <QColor>
#include <QDebug>
#include <QDragMoveEvent>
#include <QFileDialog>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QPalette>
#include <QPushButton>
#include <QSizePolicy>
#include <QTextCursor>

#include "NewPollDialog.h"
#include "OutgoingAttachmentList.h"
#include "backend/Backend.h"
#include "backend/PostCreateService.h"
#include "backend/PostProps.h"
#include "backend/types/BackendPost.h"
#include "chat-area/ChatLogWidget.h"
#include "chat-area/QuotedReplyFormat.h"
#include "choose-emoji-dialog/ChooseEmojiDialogWrapper.h"

namespace Mattermost {
namespace {

constexpr char EditingPostProperty[] = "_mmqt_editing_post";

}

struct OutgoingPostData {
	const BackendPost* postToEdit;
	std::unique_ptr<BackendNewPollData> pollData;
	QString message;
	QString replyToPostId;
	QList<QString> attachmentPaths;
	QList<QString> attachmentIds;
};

OutgoingPostCreator::OutgoingPostCreator(QWidget* parent)
    : MessageTextEditWidget(parent)
    , postToEdit(nullptr)
    , attachmentList(nullptr)
    , isConnected(true)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMinimumWidth(100);
    setContextMenuPolicy(Qt::NoContextMenu);
    setAcceptRichText(false);
    setPlaceholderText(tr("Write a message"));
}

void OutgoingPostCreator::init(Backend& backendInstance,
                               BackendChannel& channelInstance,
                               ChatLogWidget& chatLogWidget,
                               QBoxLayout* attachmentParentLayout,
                               QLabel& statusLabelInstance,
                               QPushButton& attachButtonInstance,
                               QPushButton& addEmojiButtonInstance,
                               QPushButton& sendButtonInstance)
{
	backend = &backendInstance;
	channel = &channelInstance;
	attachmentParent = attachmentParentLayout;
	statusLabel = &statusLabelInstance;
	attachButton = &attachButtonInstance;
	addEmojiButton = &addEmojiButtonInstance;
	sendButton = &sendButtonInstance;

	connect(this, &MessageTextEditWidget::escapePressed, this, [this] {
		if (!isEditingPost()
		    && !property(PostProps::ReplyToPostId).toString().isEmpty()) {
			setProperty(PostProps::ReplyToPostId, QString());
			return;
		}
		clear();
		postToEdit = nullptr;
		setEditingVisual(false);
		emit postEditFinished();
	});

	connect(this, &MessageTextEditWidget::upArrowPressed,
	        &chatLogWidget, &ChatLogWidget::editLastOwnPost);

	connect(this, &QTextEdit::textChanged,
	        this, &OutgoingPostCreator::updateSendButtonState);

	connect(sendButton, &QPushButton::clicked,
	        this, &OutgoingPostCreator::sendPostButtonAction);
	connect(this, &MessageTextEditWidget::enterPressed,
	        this, &OutgoingPostCreator::sendPostButtonAction);
	connect(attachButton, &QPushButton::clicked,
	        this, &OutgoingPostCreator::onAttachButtonClick);

	connect(addEmojiButton, &QPushButton::clicked, this, [this] {
		showEmojiDialog([this](Emoji emoji) {
			insertPlainText(" :" + emoji.name + ": ");
			setFocus();
		});
	});

	setEditingVisual(false);
	updateSendButtonState();

	connectLambda(&backendInstance, &Backend::onWebSocketConnect, [this] {
		isConnected = true;
		updateSendButtonState();
	});

	connectLambda(&backendInstance, &Backend::onWebSocketDisconnect, [this] {
		isConnected = false;
		updateSendButtonState();
	});

	connect(&sendRetryTimer, &QTimer::timeout, this, [this] {
		qDebug() << "Post send retry";
		prepareAndSendPost();
	});
}

OutgoingPostCreator::~OutgoingPostCreator()
{
	for (auto& connection : signalConnections) {
		disconnect(connection);
	}
}

void OutgoingPostCreator::setStatusLabelText(const QString& string)
{
	if (statusLabel) {
		statusLabel->setText(string);
	}
}

void OutgoingPostCreator::onAttachButtonClick()
{
	QStringList files = QFileDialog::getOpenFileNames(this, "Select File(s) to attach");
	if (files.empty()) {
		return;
	}
	createAttachmentList(files);
}

void OutgoingPostCreator::postEditInitiated(BackendPost& post)
{
	if (isCreatingPost()) {
		qDebug() << "Post edit requested while creating post";
		emit postEditFinished();
		return;
	}

	// Editing and quoted reply are mutually exclusive composer modes. Keep the
	// interoperability blockquote out of the editor; it is restored on send.
	setProperty(PostProps::ReplyToPostId, QString());
	postToEdit = &post;
	setText(QuotedReplyFormat::stripFallback(post.message));
	setFocus();
	moveCursor(QTextCursor::End);
	setEditingVisual(true);
	updateSendButtonState();
}

void OutgoingPostCreator::setEditingVisual(bool editing)
{
	setProperty(EditingPostProperty, editing);
	if (!statusLabel) {
		return;
	}

	if (editing) {
		const QColor accent = palette().color(QPalette::Highlight);
		setPlaceholderText(tr("Editing message"));
		setStyleSheet(
			QStringLiteral("QTextEdit { border: 2px solid %1; border-radius: 4px; padding: 2px; }")
				.arg(accent.name()));
		statusLabel->setText(tr("Editing message · Esc to cancel"));
		statusLabel->setStyleSheet(QStringLiteral("font-weight: 600;"));
	} else {
		setPlaceholderText(tr("Write a message"));
		setStyleSheet(QString());
		statusLabel->setStyleSheet(QString());
		if (!outgoingPostData) {
			statusLabel->clear();
		}
	}

	updateSendButtonState();
}

bool OutgoingPostCreator::isEditingPost() const
{
	return postToEdit
		|| (outgoingPostData && outgoingPostData->postToEdit);
}

static NewPollDialog* newPollDialog;

static QString getStringInsideQuotes(const QString& str, int& nextPos)
{
	const int firstQuote = str.indexOf('"', nextPos);
	if (firstQuote == -1) {
		return QString();
	}

	const int lastQuote = str.indexOf('"', firstQuote + 1);
	if (lastQuote == -1) {
		return QString();
	}

	nextPos = lastQuote + 1;
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
	return QStringRef(&str, firstQuote + 1, lastQuote - firstQuote - 1).toString();
#else
	QStringView view(str);
	return view.sliced(firstQuote + 1, lastQuote - firstQuote - 1).toString();
#endif
}

void OutgoingPostCreator::sendPostButtonAction()
{
	const QString message = toPlainText();
	if (message.isEmpty() && !attachmentList) {
		return;
	}

	const QString replyToPostId = property(PostProps::ReplyToPostId).toString();
	if (message.startsWith("/poll")) {
		if (!replyToPostId.isEmpty()) {
			QMessageBox::warning(this, tr("Cannot quote poll"),
			                     tr("Polls cannot be sent as quoted replies. Cancel the reply first."),
			                     QMessageBox::Ok);
			return;
		}
		if (postToEdit) {
			QMessageBox::warning(this, "Error",
			                     "Cannot replace a non-poll message with a poll. Either delete the post or add the poll as a new post",
			                     QMessageBox::Ok);
			return;
		}

		BackendNewPollData initialPollData;
		int startPos = 6;
		QString str = getStringInsideQuotes(message, startPos);
		qDebug() << "got" << str << "pos" << startPos;

		if (!str.isEmpty()) {
			initialPollData.question = str;
		}

		str = getStringInsideQuotes(message, startPos);
		while (!str.isEmpty()) {
			initialPollData.options.push_back(str);
			str = getStringInsideQuotes(message, startPos);
		}

		if (message.indexOf("--progress", startPos) != -1) {
			initialPollData.showProgress = true;
		}
		if (message.indexOf("--anonymous", startPos) != -1) {
			initialPollData.isAnonymous = true;
		}
		if (message.indexOf("--public-add-option", startPos) != -1) {
			initialPollData.allowAddOptions = true;
		}

		newPollDialog = new NewPollDialog(this, initialPollData);
		connect(newPollDialog, &QDialog::accepted, this, [this] {
			outgoingPostData = std::make_unique<OutgoingPostData>();
			outgoingPostData->pollData =
				std::make_unique<BackendNewPollData>(newPollDialog->getData());
			startSendPostSequence();
		});
		newPollDialog->show();
		return;
	}

	outgoingPostData = std::make_unique<OutgoingPostData>();
	outgoingPostData->message = message;
	outgoingPostData->postToEdit = postToEdit;
	if (!outgoingPostData->postToEdit) {
		outgoingPostData->replyToPostId = replyToPostId;
	}
	postToEdit = nullptr;

	if (attachmentList) {
		outgoingPostData->attachmentPaths = attachmentList->getAllFiles();
		attachmentList->setDisableInput(true);
	}

	startSendPostSequence();
}

void OutgoingPostCreator::startSendPostSequence()
{
	sendRetryTimer.start(25000);
	setReadOnly(true);
	updateSendButtonState();
	prepareAndSendPost();
	setStatusLabelText(isEditingPost() ? tr("Saving edit…") : tr("Sending message…"));
}

void OutgoingPostCreator::prepareAndSendPost()
{
	if (outgoingPostData->attachmentPaths.isEmpty()) {
		sendPost();
		return;
	}

	for (auto it = outgoingPostData->attachmentPaths.begin();
	     it != outgoingPostData->attachmentPaths.end(); ++it) {
		auto& file = *it;
		backend->uploadFile(*channel, file, [this, it](QString fileId) {
			outgoingPostData->attachmentIds.push_back(fileId);
			outgoingPostData->attachmentPaths.erase(it);
			const qsizetype uploadedFilesCount = outgoingPostData->attachmentIds.size();
			const qsizetype remainingFileCount = outgoingPostData->attachmentPaths.size();

			setStatusLabelText("Attached file " + QString::number(uploadedFilesCount)
			                   + " of " + QString::number(uploadedFilesCount + remainingFileCount));

			qDebug() << "Remaining file count:" << remainingFileCount;
			if (remainingFileCount == 0) {
				sendPost();
			}
		});
	}
}

void OutgoingPostCreator::sendPost()
{
	const QString attachmentsLogStr(outgoingPostData->attachmentIds.isEmpty()
	                                    ? "" : " (+attachments)");

	if (outgoingPostData->postToEdit) {
		qDebug() << "Send post edit" << attachmentsLogStr;
		QString wireMessage = outgoingPostData->message;
		const QString existingFallback =
			QuotedReplyFormat::fallbackPrefix(outgoingPostData->postToEdit->message);
		if (!existingFallback.isEmpty()) {
			wireMessage.prepend(existingFallback);
		}
		backend->editPost(outgoingPostData->postToEdit->id,
		                  wireMessage,
		                  outgoingPostData->attachmentIds);
	} else if (outgoingPostData->pollData) {
		backend->addPoll(*channel, *outgoingPostData->pollData);
	} else {
		qDebug() << "Send post" << attachmentsLogStr;
		QJsonObject props;
		QString wireMessage = outgoingPostData->message;
		if (!outgoingPostData->replyToPostId.isEmpty()) {
			props.insert(PostProps::ReplyToPostId, outgoingPostData->replyToPostId);

			if (BackendPost* quotedPost = channel->postIdToPost.value(
			        outgoingPostData->replyToPostId, nullptr)) {
				wireMessage.prepend(QuotedReplyFormat::buildFallback(
					quotedPost->id,
					quotedPost->getDisplayAuthorName(),
					quotedPost->message,
					!quotedPost->files.empty()));
			} else {
				qWarning() << "Quoted reply target is not materialized:"
				           << outgoingPostData->replyToPostId;
			}
		}
		PostCreateService::instance(*backend).createPost(
			*channel, wireMessage, outgoingPostData->attachmentIds,
			root_id, props);
	}
}

void OutgoingPostCreator::onDragEnterEvent(QDragEnterEvent* event)
{
	if (event->mimeData()->hasUrls()) {
		event->acceptProposedAction();
	}
}

void OutgoingPostCreator::onDragMoveEvent(QDragMoveEvent* event)
{
	if (event->mimeData()->hasUrls()) {
		event->acceptProposedAction();
	}
}

void OutgoingPostCreator::onDropEvent(QDropEvent* event)
{
	if (isWaitingForPostServerResponse()) {
		qDebug() << "Cannot attach files while sending a post";
		return;
	}

	QStringList files;
	for (auto& url : event->mimeData()->urls()) {
		files.push_back(url.toLocalFile());
	}
	createAttachmentList(files);
}

void OutgoingPostCreator::onPostReceived(BackendPost& post)
{
	if (!outgoingPostData || (!post.isOwnPost() && !post.isOwnPollPost())) {
		return;
	}

	sendRetryTimer.stop();
	const bool wasEdit = outgoingPostData->postToEdit != nullptr;

	if (wasEdit) {
		emit postEditFinished();
	}

	outgoingPostData.reset();
	setProperty(PostProps::ReplyToPostId, QString());

	if (attachmentList) {
		delete attachmentList;
		attachmentList = nullptr;
	}

	clear();
	setReadOnly(false);
	setEditingVisual(false);
	setStatusLabelText(QString());
	updateSendButtonState();
}

void OutgoingPostCreator::createAttachmentList(QStringList& files)
{
	if (!attachmentList) {
		attachmentList = new OutgoingAttachmentList(this);
		attachmentParent->insertWidget(0, attachmentList);
		updateSendButtonState();

		connect(attachmentList, &OutgoingAttachmentList::deleted, this, [this] {
			attachmentParent->removeWidget(attachmentList);
			delete attachmentList;
			attachmentList = nullptr;
			updateSendButtonState();
		});
	}

	for (auto& filename : files) {
		attachmentList->addFile(filename);
	}
}

void OutgoingPostCreator::updateSendButtonState()
{
	if (!sendButton || !attachButton) {
		return;
	}

	const bool editing = isEditingPost();
	sendButton->setText(editing ? QStringLiteral("✓") : QStringLiteral("➤"));
	sendButton->setAccessibleName(editing ? tr("Save edited message") : tr("Send"));

	bool sendButtonEnabled = true;
	QString tooltipText;

	if (!isConnected) {
		tooltipText = tr("Server connection lost");
		if (outgoingPostData) {
			tooltipText += tr(", sending message");
		}
		sendButtonEnabled = false;
	} else if (outgoingPostData) {
		sendButtonEnabled = false;
		tooltipText = editing ? tr("Saving edited message") : tr("Waiting for server response");
	}

	attachButton->setDisabled(!sendButtonEnabled);
	attachButton->setToolTip(tooltipText.isEmpty() ? tr("Attach File") : tooltipText);

	if (sendButtonEnabled && !isCreatingPost()) {
		sendButtonEnabled = false;
		tooltipText = tr("Cannot send empty message");
	}

	if (sendButtonEnabled && editing) {
		tooltipText = tr("Save edited message · Esc to cancel");
	} else if (sendButtonEnabled && tooltipText.isEmpty()) {
		tooltipText = tr("Send");
	}

	sendButton->setDisabled(!sendButtonEnabled);
	sendButton->setToolTip(tooltipText);
}

bool OutgoingPostCreator::isCreatingPost()
{
	if (isWaitingForPostServerResponse()) {
		return true;
	}

	const QString message = toPlainText();
	return !message.isEmpty() || attachmentList;
}

bool OutgoingPostCreator::isWaitingForPostServerResponse()
{
	return outgoingPostData != nullptr;
}

void OutgoingPostCreator::setRootId(QString id)
{
	root_id = std::move(id);
}

} /* namespace Mattermost */