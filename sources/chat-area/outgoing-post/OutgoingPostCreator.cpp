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

#include "OutgoingPostCreator.h"

#include <QColor>
#include <QDebug>
#include <QDragMoveEvent>
#include <QFileDialog>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QPalette>
#include <QPushButton>
#include <QTextCursor>

#include "NewPollDialog.h"
#include "OutgoingAttachmentList.h"
#include "OutgoingPostPanel.h"
#include "backend/Backend.h"
#include "chat-area/ChatLogWidget.h"
#include "choose-emoji-dialog/ChooseEmojiDialogWrapper.h"
#include "ui_OutgoingPostCreator.h"

namespace Mattermost {

struct OutgoingPostData {
	const BackendPost* postToEdit;
	std::unique_ptr<BackendNewPollData> pollData;
	QString message;
	QList<QString> attachmentPaths;
	QList<QString> attachmentIds;
};

OutgoingPostCreator::OutgoingPostCreator(QWidget* parent)
    : QWidget(parent)
    , ui(new Ui::OutgoingPostCreator)
    , postToEdit(nullptr)
    , attachmentList(nullptr)
    , isConnected(true)
{
    ui->setupUi(this);
}

void OutgoingPostCreator::init(Backend& backendInstance,
                               BackendChannel& channelInstance,
                               OutgoingPostPanel& panelInstance,
                               ChatLogWidget& chatLogWidget,
                               QBoxLayout* attachmentParentLayout)
{
	backend = &backendInstance;
	channel = &channelInstance;
	panel = &panelInstance;
	attachmentParent = attachmentParentLayout;

	connect(ui->textEdit, &MessageTextEditWidget::escapePressed, [this] {
		ui->textEdit->clear();
		postToEdit = nullptr;
		setEditingVisual(false);
		emit postEditFinished();
	});

	connect(ui->textEdit, &MessageTextEditWidget::upArrowPressed,
	        &chatLogWidget, &ChatLogWidget::editLastOwnPost);

	connect(ui->textEdit, &MessageTextEditWidget::textChanged, [this] {
		updateSendButtonState();

		int height = ui->textEdit->document()->size().toSize().height();
		if (height > ui->textEdit->maximumHeight()) {
			height = ui->textEdit->maximumHeight();
		}
		emit heightChanged(height);
	});

	connect(&panelInstance.sendButton(), &QPushButton::clicked,
	        this, &OutgoingPostCreator::sendPostButtonAction);
	connect(ui->textEdit, &MessageTextEditWidget::enterPressed,
	        this, &OutgoingPostCreator::sendPostButtonAction);
	connect(&panelInstance.attachButton(), &QPushButton::clicked,
	        this, &OutgoingPostCreator::onAttachButtonClick);

	connect(&panelInstance.addEmojiButton(), &QPushButton::clicked, [this] {
		showEmojiDialog([this](Emoji emoji) {
			auto* textEdit = ui->textEdit;
			textEdit->insertPlainText(" :" + emoji.name + ": ");
			textEdit->setFocus();
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

	connect(&sendRetryTimer, &QTimer::timeout, [this] {
		qDebug() << "Post send retry";
		prepareAndSendPost();
	});
}

OutgoingPostCreator::~OutgoingPostCreator()
{
	for (auto& connection : signalConnections) {
		disconnect(connection);
	}
	delete ui;
}

void OutgoingPostCreator::setStatusLabelText(const QString& string)
{
	panel->label().setText(string);
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

	postToEdit = &post;
	ui->textEdit->setText(post.message);
	ui->textEdit->setFocus();
	ui->textEdit->moveCursor(QTextCursor::End);
	setEditingVisual(true);
	updateSendButtonState();
}

void OutgoingPostCreator::setEditingVisual(bool editing)
{
	if (!panel) {
		return;
	}

	if (editing) {
		const QColor accent = palette().color(QPalette::Highlight);
		ui->textEdit->setPlaceholderText(tr("Editing message"));
		ui->textEdit->setStyleSheet(
			QStringLiteral("QTextEdit { border: 2px solid %1; border-radius: 4px; padding: 2px; }")
				.arg(accent.name()));
		panel->label().setText(tr("Editing message · Esc to cancel"));
		panel->label().setStyleSheet(QStringLiteral("font-weight: 600;"));
	} else {
		ui->textEdit->setPlaceholderText(tr("Write a message"));
		ui->textEdit->setStyleSheet(QString());
		panel->label().setStyleSheet(QString());
		if (!outgoingPostData) {
			panel->label().clear();
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
	const QString message = ui->textEdit->toPlainText();
	if (message.isEmpty() && !attachmentList) {
		return;
	}

	if (message.startsWith("/poll")) {
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
		connect(newPollDialog, &QDialog::accepted, [this] {
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
	ui->textEdit->setReadOnly(true);
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
		backend->editPost(outgoingPostData->postToEdit->id,
		                  outgoingPostData->message,
		                  outgoingPostData->attachmentIds);
	} else if (outgoingPostData->pollData) {
		backend->addPoll(*channel, *outgoingPostData->pollData);
	} else {
		qDebug() << "Send post" << attachmentsLogStr;
		backend->addPost(*channel, outgoingPostData->message,
		                 outgoingPostData->attachmentIds, root_id);
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

	if (attachmentList) {
		delete attachmentList;
		attachmentList = nullptr;
	}

	ui->textEdit->clear();
	ui->textEdit->setReadOnly(false);
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

		connect(attachmentList, &OutgoingAttachmentList::deleted, [this] {
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
	if (!panel) {
		return;
	}

	const bool editing = isEditingPost();
	panel->sendButton().setText(editing ? QStringLiteral("✓") : QStringLiteral("➤"));
	panel->sendButton().setAccessibleName(editing ? tr("Save edited message") : tr("Send"));

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

	panel->attachButton().setDisabled(!sendButtonEnabled);
	panel->attachButton().setToolTip(tooltipText.isEmpty() ? tr("Attach File") : tooltipText);

	if (sendButtonEnabled && !isCreatingPost()) {
		sendButtonEnabled = false;
		tooltipText = tr("Cannot send empty message");
	}

	if (sendButtonEnabled && editing) {
		tooltipText = tr("Save edited message · Esc to cancel");
	} else if (sendButtonEnabled && tooltipText.isEmpty()) {
		tooltipText = tr("Send");
	}

	panel->sendButton().setDisabled(!sendButtonEnabled);
	panel->sendButton().setToolTip(tooltipText);
}

bool OutgoingPostCreator::isCreatingPost()
{
	if (isWaitingForPostServerResponse()) {
		return true;
	}

	const QString message = ui->textEdit->toPlainText();
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
