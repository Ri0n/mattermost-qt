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

#include <QDateTime>
#include <QDebug>
#include <QEvent>
#include <QPalette>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QTextBrowser>
#include <QUrl>

#include "MessageContentWidget.h"
#include "MessageFormatter.h"
#include "PostQuoteFrame.h"
#include "ThreadSummaryWidget.h"
#include "attachments/PostAttachmentList.h"
#include "attachments/PostPoll.h"
#include "backend/Backend.h"
#include "backend/UserProfileService.h"
#include "backend/emoji/EmojiInfo.h"
#include "backend/types/BackendPost.h"
#include "chat-area/ChatArea.h"
#include "chat-area/ThreadWindowTitle.h"
#include "info-dialogs/UserProfileDialog.h"
#include "navigation/AppNavigationService.h"
#include "reactions/PostReactionList.h"
#include "ui/AvatarUtils.h"
#include "ui_PostWidget.h"

namespace Mattermost {

PostWidget::PostWidget(Backend& backend,
                       BackendPost& post,
                       QWidget* parent,
                       ChatArea* chatArea,
                       BackendPost* lastRootPost)
    : QWidget(parent)
    , post(post)
    , threadButton(nullptr)
    , backend(backend)
    , ui(new Ui::PostWidget)
    , messageContent(nullptr)
    , parentChatArea(chatArea)
{
	ui->setupUi(this);
	ui->authorAvatar->setFrameShape(QFrame::NoFrame);
	ui->authorName->setText(post.getDisplayAuthorName());

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
	connectMessageLinks();
	ui->time->setText(getMessageTimeString(post.create_at));

	connect(messageContent, &MessageContentWidget::linkHovered,
	        this, [this](const QString& link) {
		qDebug() << "Link hovered:" << link;
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

	if (post.rootPost && post.rootPost != lastRootPost) {
		quoteFrame = std::make_unique<PostQuoteFrame>(*post.rootPost,
		                                              backend.getStorage(), this);
		ui->verticalLayout->insertWidget(1, quoteFrame.get(), 0, Qt::AlignLeft);
		connect(quoteFrame.get(), &PostQuoteFrame::postClicked, [&post, chatArea] {
			chatArea->goToPost(*post.rootPost);
		});
	}

	if (!post.files.empty()) {
		attachments = std::make_unique<PostAttachmentList>(backend, this);
		connect(attachments.get(), &PostAttachmentList::dimensionsChanged,
		        this, &PostWidget::dimensionsChanged);
		ui->verticalLayout->addWidget(attachments.get(), 0, Qt::AlignLeft);
		for (const BackendFile& file : post.files) {
			attachments->addFile(file, post.getDisplayAuthorName());
		}
	}

	if (!post.reactions.empty()) {
		reactions = std::make_unique<PostReactionList>(this);
		for (auto& it : post.reactions) {
			const EmojiID emojiID = it.first;
			const Emoji emoji = EmojiInfo::getEmoji(emojiID);
			reactions->addReaction(emoji.name, emoji.unicodeString, it.second);
		}
		connectReactionActions();
		ui->verticalLayout->addWidget(reactions.get(), 0, Qt::AlignLeft);
	}

	if (post.poll) {
		clearMessageText();
		poll = std::make_unique<PostPoll>(backend, post, *post.poll, this);
		ui->verticalLayout->addWidget(poll.get(), 0, Qt::AlignLeft);
	}

	if (parentChatArea && !parentChatArea->isThread) {
		addThreadButton();
	}
}

PostWidget::~PostWidget()
{
	delete ui;
}

void PostWidget::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (!event || (event->type() != QEvent::PaletteChange
                   && event->type() != QEvent::ApplicationPaletteChange)) {
        return;
    }

    updateAuthorAvatar();
    update();
    const auto childWidgets = findChildren<QWidget*>();
    for (QWidget* child : childWidgets) {
        if (child) {
            child->update();
        }
    }
    if (QWidget* viewportWidget = parentWidget()) {
        viewportWidget->update();
    }
}

void PostWidget::setAuthor(Backend& backendInstance, const BackendUser* user)
{
	if (!user) {
		return;
	}

	post.author = user;
	ui->authorName->setText(post.getDisplayAuthorName());
	if (post.isOwnPost()) {
		ui->authorName->setStyleSheet("QLabel { color : blue; }");
	}

	connect(user, &BackendUser::onAvatarChanged,
	        this, &PostWidget::updateAuthorAvatar, Qt::UniqueConnection);
	connect(user, &BackendUser::onStatusChanged,
	        this, &PostWidget::updateAuthorAvatar, Qt::UniqueConnection);

	if (user->avatar.isNull()
		|| user->avatar_picture_update != user->last_picture_update) {
		UserProfileService::instance(backendInstance).ensureAvatar(*user);
	} else {
		updateAuthorAvatar();
	}

	if (user->status.isEmpty()) {
		QPointer<PostWidget> guard(this);
		UserProfileService::instance(backendInstance).ensureStatuses(
			QStringList {user->id}, [guard] {
				if (guard) {
					guard->updateAuthorAvatar();
				}
			});
	}
}

void PostWidget::updateAuthorAvatar()
{
	if (!post.author || post.author->avatar.isNull()) {
		ui->authorAvatar->clear();
		return;
	}

	ui->authorAvatar->setPixmap(
		AvatarUtils::withStatus(post.author->avatar, 48, post.author->status, 12,
		                        palette().color(QPalette::Window)));
}

void PostWidget::setEdited(const QString& message)
{
	messageContent->setMessage(message);
	connectMessageLinks();

	if (post.poll) {
		clearMessageText();
		std::unique_ptr<PostPoll> newPoll =
			std::make_unique<PostPoll>(poll->backend, post, *post.poll, this);
		ui->verticalLayout->replaceWidget(poll.get(), newPoll.get());
		poll = std::move(newPoll);
	}
}

void PostWidget::connectMessageLinks()
{
	const auto browsers = messageContent->findChildren<QTextBrowser*>();
	for (QTextBrowser* browser : browsers) {
		if (!browser) {
			continue;
		}

		browser->setOpenLinks(false);
		browser->setOpenExternalLinks(false);
		connect(browser, &QTextBrowser::anchorClicked, this,
		        [this](const QUrl& url) {
            if (url.scheme() == QStringLiteral("mattermost-user")) {
                QString username = url.path();
                while (username.startsWith(QLatin1Char('/'))) {
                    username.remove(0, 1);
                }
                if (!username.isEmpty()) {
                    openUserProfile(username);
                }
                return;
            }
			AppNavigationService::instance(backend).openUrl(url);
		});
	}
}

void PostWidget::openUserProfile(const QString& username)
{
    const auto showProfile = [this](const BackendUser* user) {
        if (!user) {
            return;
        }
        auto* dialog = new UserProfileDialog(backend, *user, this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
    };

    for (const auto& entry : backend.getStorage().getAllUsers()) {
        const BackendUser& user = entry.second;
        if (user.username.compare(username, Qt::CaseInsensitive) == 0) {
            showProfile(&user);
            return;
        }
    }

    UserSearchOptions options;
    options.term = username;
    options.limit = 20;

    QPointer<PostWidget> guard(this);
    UserProfileService::instance(backend).searchUsers(
        options, [guard, username](QVector<const BackendUser*> users) {
            if (!guard) {
                return;
            }
            for (const BackendUser* user : users) {
                if (user && user->username.compare(username, Qt::CaseInsensitive) == 0) {
                    guard->openUserProfile(user->username);
                    return;
                }
            }
        });
}

void PostWidget::updateReactions()
{
	if (reactions) {
		reactions.reset();
	}

	if (!post.reactions.empty()) {
		reactions = std::make_unique<PostReactionList>(this);
		for (auto& it : post.reactions) {
			const EmojiID emojiID = it.first;
			const Emoji emoji = EmojiInfo::getEmoji(emojiID);
			reactions->addReaction(emoji.name, emoji.unicodeString, it.second);
		}
		connectReactionActions();
		ui->verticalLayout->addWidget(reactions.get(), 0, Qt::AlignLeft);
	}
}

void PostWidget::connectReactionActions()
{
	if (!reactions) {
		return;
	}

	connect(reactions.get(), &PostReactionList::reactionClicked,
	        this, [this](const QString& emojiName) {
		backend.addPostReaction(post.id, emojiName);
	});
}

void PostWidget::addThreadButton()
{
	if (!threadSummary) {
		threadSummary = new ThreadSummaryWidget(backend,
		                                        parentChatArea->getChannel(),
		                                        post, this);
		connect(threadSummary, &ThreadSummaryWidget::clicked,
		        this, &PostWidget::openThreadWindow);

		ui->time->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
		ui->horizontalLayout->insertStretch(1, 1);
		ui->horizontalLayout->insertWidget(2, threadSummary, 0, Qt::AlignVCenter);
		const int threadTimeGap = ui->time->fontMetrics().averageCharWidth();
		ui->horizontalLayout->insertSpacing(3, threadTimeGap);
		return;
	}

	threadSummary->refresh();
}

void PostWidget::openThreadWindow()
{
	ChatArea* area;
	if (parentChatArea->threadsAreas.empty()) {
		area = new ChatArea(parentChatArea->backend, parentChatArea->channel,
		                    post.id, parentChatArea);
		area->root_id = post.id;
        area->setWindowTitle(threadWindowTitle(parentChatArea->channel, post));
		parentChatArea->threadsAreas.insert(area);
		area->show();
	} else {
		auto it = parentChatArea->threadsAreas.begin();
		const auto end = parentChatArea->threadsAreas.end();
		for (; it != end; ++it) {
			if ((*it)->root_id == post.id) {
                (*it)->setWindowTitle(threadWindowTitle(parentChatArea->channel, post));
				(*it)->activateWindow();
				qDebug() << "exists";
				break;
			}
		}
		if (it == parentChatArea->threadsAreas.end()) {
			area = new ChatArea(parentChatArea->backend, parentChatArea->channel,
			                    post.id, parentChatArea);
			area->root_id = post.id;
            area->setWindowTitle(threadWindowTitle(parentChatArea->channel, post));
			parentChatArea->threadsAreas.insert(area);
			area->show();
		}
	}

	qDebug() << post.id << post.has_thread << post.hidden << post.root_id;
}

void PostWidget::markAsDeleted()
{
	attachments.reset(nullptr);
	post.isDeleted = true;
	if (poll) {
		ui->verticalLayout->removeWidget(poll.get());
		poll.reset(nullptr);
		messageContent->setMessage(QStringLiteral("(Poll deleted)"));
	} else {
		messageContent->setMessage(QStringLiteral("(Message deleted)"));
	}
}

QString PostWidget::getSelectedText()
{
	return messageContent->selectedText();
}

QString PostWidget::formatMessageText(const QString& str)
{
	return MessageFormatter::formatMessageText(str);
}

QString PostWidget::getMessageTimeString(uint64_t timestamp)
{
	const QDate currentDate = QDateTime::currentDateTime().date();
	const QDateTime postTime = QDateTime::fromMSecsSinceEpoch(timestamp);
	const QDate postDate = postTime.date();

	QString format;
	if (currentDate.year() != postDate.year()) {
		format = "dd MMM yyyy, hh:mm:ss";
	} else if (currentDate.day() != postDate.day()
	           || currentDate.month() != postDate.month()) {
		format = "dd MMM, hh:mm:ss";
	} else {
		format = "hh:mm:ss";
	}

	return postTime.toString(format);
}

QString PostWidget::formatForClipboardSelection(FormatType formatType) const
{
	if (formatType == messageOnly) {
		return post.message;
	}

	QString ret(post.getDisplayAuthorName() + "\t[" + ui->time->text() + "]\n");
	ret += " " + post.message + "\n\n";
	return ret;
}

void PostWidget::clearMessageText()
{
	messageContent->clear();
}

} /* namespace Mattermost */
