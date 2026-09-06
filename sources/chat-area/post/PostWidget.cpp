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

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDateTime>
#include <QDebug>
#include <QEvent>
#include <QMenu>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QTextBrowser>
#include <QUrl>

#include "MessageContentWidget.h"
#include "MessageFormatter.h"
#include "PostQuoteFrame.h"
#include "ThreadSummaryWidget.h"
#include "UserMentionLinkifier.h"
#include "attachments/PostAttachmentList.h"
#include "attachments/PostPoll.h"
#include "backend/Backend.h"
#include "backend/MentionGroupService.h"
#include "backend/PostProps.h"
#include "backend/PostRepository.h"
#include "backend/UserProfileService.h"
#include "backend/emoji/EmojiInfo.h"
#include "backend/types/BackendPost.h"
#include "chat-area/ChatArea.h"
#include "chat-area/QuotedPostPreview.h"
#include "chat-area/QuotedReplyController.h"
#include "chat-area/QuotedReplyFormat.h"
#include "chat-area/ThreadWindowTitle.h"
#include "choose-emoji-dialog/ChooseEmojiDialogWrapper.h"
#include "info-dialogs/UserProfileDialog.h"
#include "navigation/AppNavigationService.h"
#include "reactions/PostReactionList.h"
#include "ui/AvatarUtils.h"
#include "ui_PostWidget.h"

namespace Mattermost {

namespace {

QString internalLinkValue(const QUrl& url)
{
    QString value = url.path();
    while (value.startsWith(QLatin1Char('/'))) {
        value.remove(0, 1);
    }
    return value;
}

QString quotedPostId(const BackendPost& post)
{
    return post.props.toObject()
        .value(QString::fromLatin1(PostProps::ReplyToPostId)).toString();
}

QString displayMessage(const BackendPost& post, const QString& wireMessage)
{
    if (post.isDeleted) {
        return post.poll ? QStringLiteral("(Poll deleted)")
                         : QStringLiteral("(Message deleted)");
    }
    return quotedPostId(post).isEmpty()
        ? wireMessage : QuotedReplyFormat::stripFallback(wireMessage);
}

} // namespace

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
	messageContent->setMessage(displayMessage(post, post.message));
	connectMessageLinks();
	ui->time->setText(getMessageTimeString(post.create_at));

	connect(messageContent, &MessageContentWidget::linkHovered,
	        this, [this](const QString& link) {
		qDebug() << "Link hovered:" << link;
		hoveredLink = link;
	});

    const QString teamId = mentionTeamId();
    if (!teamId.isEmpty()) {
        auto& groupService = MentionGroupService::instance(backend);
        connect(&groupService, &MentionGroupService::groupsChanged,
                this, [this, teamId](const QString& changedTeamId) {
            if (changedTeamId == teamId) {
                refreshMentionLinks();
            }
        });
        groupService.ensureTeamGroups(teamId);
    }

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

    const QString replyPostId = quotedPostId(post);
    if (!post.isDeleted && !replyPostId.isEmpty() && parentChatArea) {
        BackendPost* quotedPost = parentChatArea->channel.postIdToPost.value(replyPostId, nullptr);
        if (quotedPost && quotedPost != &post) {
            quotedReplyPreview = std::make_unique<QuotedPostPreview>(this, 2);
            quotedReplyPreview->setPost(*quotedPost);
            quotedReplyPreview->setActivatedCallback([this, replyPostId] {
                if (parentChatArea) {
                    parentChatArea->goToPost(replyPostId);
                }
            });
            ui->verticalLayout->insertWidget(1, quotedReplyPreview.get());
        } else {
            QPointer<PostWidget> guard(this);
            PostRepository::instance(backend).loadPost(
                replyPostId,
                [guard, replyPostId](const PostRepository::PostResult& result) {
                    if (!guard || !result.success || !guard->parentChatArea
                        || guard->quotedReplyPreview || guard->post.isDeleted) {
                        return;
                    }
                    BackendPost* loaded = guard->parentChatArea->channel.postIdToPost
                        .value(replyPostId, nullptr);
                    if (!loaded || loaded == &guard->post) {
                        return;
                    }

                    guard->quotedReplyPreview =
                        std::make_unique<QuotedPostPreview>(guard, 2);
                    guard->quotedReplyPreview->setPost(*loaded);
                    guard->quotedReplyPreview->setActivatedCallback(
                        [guard, replyPostId] {
                            if (guard && guard->parentChatArea) {
                                guard->parentChatArea->goToPost(replyPostId);
                            }
                        });
                    guard->ui->verticalLayout->insertWidget(
                        1, guard->quotedReplyPreview.get());
                    emit guard->dimensionsChanged();
                });
        }
	} else if (!post.isDeleted && post.rootPost && post.rootPost != lastRootPost) {
		quoteFrame = std::make_unique<PostQuoteFrame>(*post.rootPost,
		                                              backend.getStorage(), this);
		ui->verticalLayout->insertWidget(1, quoteFrame.get(), 0, Qt::AlignLeft);
		connect(quoteFrame.get(), &PostQuoteFrame::postClicked, [&post, chatArea] {
			chatArea->goToPost(*post.rootPost);
		});
	}

	if (!post.isDeleted && !post.files.empty()) {
		attachments = std::make_unique<PostAttachmentList>(backend, this);
		connect(attachments.get(), &PostAttachmentList::dimensionsChanged,
		        this, &PostWidget::dimensionsChanged);
		ui->verticalLayout->addWidget(attachments.get(), 0, Qt::AlignLeft);
		for (const BackendFile& file : post.files) {
			attachments->addFile(file, post.getDisplayAuthorName());
		}
	}

	if (!post.isDeleted && !post.reactions.empty()) {
		reactions = std::make_unique<PostReactionList>(this);
		for (auto& it : post.reactions) {
			const EmojiID emojiID = it.first;
			const Emoji emoji = EmojiInfo::getEmoji(emojiID);
			reactions->addReaction(emoji.name, emoji.unicodeString, it.second);
		}
		connectReactionActions();
		ui->verticalLayout->addWidget(reactions.get(), 0, Qt::AlignLeft);
	}

	if (!post.isDeleted && post.poll) {
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
    refreshMentionLinks();
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

void PostWidget::contextMenuEvent(QContextMenuEvent* event)
{
    if (!event) {
        return;
    }
    showPostContextMenu(event->globalPos());
    event->accept();
}

void PostWidget::showPostContextMenu(const QPoint& globalPos)
{
    if (post.isDeleted) {
        return;
    }

    QMenu menu(this);

    if (parentChatArea) {
        QAction* replyAction = menu.addAction(tr("Reply"));
        connect(replyAction, &QAction::triggered, this, [this] {
            if (parentChatArea) {
                QuotedReplyController::instance(*parentChatArea).begin(post);
            }
        });

        if (!parentChatArea->isThread) {
            QAction* threadAction = menu.addAction(tr("Reply in thread"));
            connect(threadAction, &QAction::triggered,
                    this, &PostWidget::openThreadWindow);
        }
        menu.addSeparator();
    }

    if (post.isOwnPost()) {
        if (parentChatArea) {
            QAction* editAction = menu.addAction(tr("Edit"));
            connect(editAction, &QAction::triggered, this, [this] {
                parentChatArea->editPost(post);
            });
        }
        QAction* deleteAction = menu.addAction(tr("Delete"));
        connect(deleteAction, &QAction::triggered, this, [this] {
            backend.deletePost(post.id);
        });
        menu.addSeparator();
    }

    if (!hoveredLink.isEmpty()) {
        QAction* copyLinkAction = menu.addAction(tr("Copy link to clipboard"));
        connect(copyLinkAction, &QAction::triggered, this, [this] {
            QApplication::clipboard()->setText(hoveredLink);
        });
    }

    const QString selectedText = getSelectedText();
    if (!selectedText.isEmpty()) {
        QAction* copySelectedAction = menu.addAction(tr("Copy selected text"));
        connect(copySelectedAction, &QAction::triggered, this, [selectedText] {
            QApplication::clipboard()->setText(selectedText);
        });
    }

    QAction* copyEntireAction = menu.addAction(tr("Copy entire post (formatted)"));
    connect(copyEntireAction, &QAction::triggered, this, [this] {
        QApplication::clipboard()->setText(formatForClipboardSelection(entirePost));
    });

    QAction* copyMessageAction = menu.addAction(tr("Copy post message"));
    connect(copyMessageAction, &QAction::triggered, this, [this] {
        QApplication::clipboard()->setText(formatForClipboardSelection(messageOnly));
    });

    QAction* reactionAction = menu.addAction(tr("Add emoji reaction"));
    connect(reactionAction, &QAction::triggered, this, [this] {
        showEmojiDialog([this](Emoji emoji) {
            backend.addPostReaction(post.id, emoji.name);
        });
    });

    if (post.author) {
        menu.addSeparator();
        QAction* profileAction = menu.addAction(
            tr("View %1's profile").arg(post.author->getDisplayName()));
        connect(profileAction, &QAction::triggered, this, [this] {
            if (!post.author) {
                return;
            }
            auto* dialog = new UserProfileDialog(backend, *post.author, this);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->show();
        });
    }

    menu.exec(globalPos);
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
	messageContent->setMessage(displayMessage(post, message));
	connectMessageLinks();

	if (post.poll) {
		clearMessageText();
		std::unique_ptr<PostPoll> newPoll =
			std::make_unique<PostPoll>(poll->backend, post, *post.poll, this);
		ui->verticalLayout->replaceWidget(poll.get(), newPoll.get());
		poll = std::move(newPoll);
	}
}

QString PostWidget::mentionTeamId() const
{
    return parentChatArea && parentChatArea->channel.team
        ? parentChatArea->channel.team->id : QString();
}

void PostWidget::refreshMentionLinks()
{
    if (!messageContent || post.poll || post.isDeleted) {
        return;
    }
    messageContent->setMessage(displayMessage(post, post.message));
    connectMessageLinks();
}

void PostWidget::connectMessageLinks()
{
    QHash<QString, QString> groupMentionIds;
    const QString teamId = mentionTeamId();
    if (!teamId.isEmpty()) {
        groupMentionIds = MentionGroupService::instance(backend).mentionIds(teamId);
    }

	const auto browsers = messageContent->findChildren<QTextBrowser*>();
	for (QTextBrowser* browser : browsers) {
		if (!browser) {
			continue;
		}

        UserMentionLinkifier::linkify(*browser->document(), groupMentionIds);

		browser->setOpenLinks(false);
		browser->setOpenExternalLinks(false);
        browser->setContextMenuPolicy(Qt::CustomContextMenu);
        QObject::disconnect(browser, nullptr, this, nullptr);
		connect(browser, &QTextBrowser::anchorClicked, this,
		        [this](const QUrl& url) {
            if (url.scheme() == QStringLiteral("mattermost-user")) {
                const QString username = internalLinkValue(url);
                if (!username.isEmpty()) {
                    openUserProfile(username);
                }
                return;
            }
            if (url.scheme() == QStringLiteral("mattermost-group")) {
                const QString groupId = internalLinkValue(url);
                if (!groupId.isEmpty()) {
                    openGroupMention(groupId);
                }
                return;
            }
			AppNavigationService::instance(backend).openUrl(url);
		});
        connect(browser, &QWidget::customContextMenuRequested, this,
                [this, browser](const QPoint& pos) {
            showPostContextMenu(browser->viewport()->mapToGlobal(pos));
        });
	}

    const auto codeEditors = messageContent->findChildren<QPlainTextEdit*>();
    for (QPlainTextEdit* editor : codeEditors) {
        if (!editor) {
            continue;
        }
        editor->setContextMenuPolicy(Qt::CustomContextMenu);
        QObject::disconnect(editor, nullptr, this, nullptr);
        connect(editor, &QWidget::customContextMenuRequested, this,
                [this, editor](const QPoint& pos) {
            showPostContextMenu(editor->viewport()->mapToGlobal(pos));
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

void PostWidget::openGroupMention(const QString& groupId)
{
    const QString teamId = mentionTeamId();
    if (teamId.isEmpty()) {
        return;
    }

    auto& service = MentionGroupService::instance(backend);
    const MentionGroup* group = service.groupById(teamId, groupId);
    const QString title = group && !group->displayName.isEmpty()
        ? group->displayName : QStringLiteral("@") + (group ? group->name : QString());

    QPointer<PostWidget> guard(this);
    service.retrieveMembers(groupId,
        [guard, title](QVector<MentionGroupMember> members) {
            if (!guard) {
                return;
            }

            auto* menu = new QMenu(guard);
            menu->setAttribute(Qt::WA_DeleteOnClose);
            if (!title.isEmpty()) {
                QAction* titleAction = menu->addAction(title);
                titleAction->setEnabled(false);
                menu->addSeparator();
            }

            if (members.isEmpty()) {
                QAction* emptyAction = menu->addAction(guard->tr("No members"));
                emptyAction->setEnabled(false);
            } else {
                for (const MentionGroupMember& member : members) {
                    QString label = member.displayName;
                    if (!member.username.isEmpty()
                        && member.displayName.compare(member.username, Qt::CaseInsensitive) != 0) {
                        label += QStringLiteral(" (@") + member.username + QLatin1Char(')');
                    }
                    QAction* action = menu->addAction(label);
                    const QString username = member.username;
                    QObject::connect(action, &QAction::triggered, guard,
                                     [guard, username] {
                        if (guard && !username.isEmpty()) {
                            guard->openUserProfile(username);
                        }
                    });
                }
            }
            menu->popup(QCursor::pos());
        });
}

void PostWidget::updateReactions()
{
	if (reactions) {
		reactions.reset();
	}

	if (post.isDeleted) {
		return;
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
	post.isDeleted = true;
	quoteFrame.reset();
	quotedReplyPreview.reset();
	attachments.reset();
	reactions.reset();
	if (poll) {
		ui->verticalLayout->removeWidget(poll.get());
		poll.reset();
		messageContent->setMessage(QStringLiteral("(Poll deleted)"));
	} else {
		messageContent->setMessage(QStringLiteral("(Message deleted)"));
	}
	emit dimensionsChanged();
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
	const QString visibleMessage = displayMessage(post, post.message);
	if (formatType == messageOnly) {
		return visibleMessage;
	}

	QString ret(post.getDisplayAuthorName() + "\t[" + ui->time->text() + "]\n");
	ret += " " + visibleMessage + "\n\n";
	return ret;
}

void PostWidget::clearMessageText()
{
	messageContent->clear();
}

} /* namespace Mattermost */