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
#include <QSignalBlocker>
#include <QPropertyAnimation>
#include <QPainter>
#include <QGraphicsOpacityEffect>
#include <QCheckBox>
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
#include "PostPermalinkUtils.h"
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
#include "integrations/KTalkMeetingWidget.h"
#include "navigation/AppNavigationService.h"
#include "reactions/PostReactionList.h"
#include "ui/AvatarUtils.h"
#include "ui/IconUtils.h"
#include "ui/EmojiFont.h"
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
    , backend_(backend)
    , residencyLease(PostRepository::instance(backend).leasePost(post))
    , ui(new Ui::PostWidget)
    , messageContent(nullptr)
    , parentChatArea(chatArea)
{
	ui->setupUi(this);

    wholeMessageCheck_ = new QCheckBox(this);
    wholeMessageCheck_->setToolTip(tr("Select message"));
    wholeMessageCheck_->setAccessibleName(tr("Select message"));
    wholeMessageCheck_->setVisible(false);
    ui->horizontalLayout_2->insertWidget(0, wholeMessageCheck_, 0, Qt::AlignTop);
    connect(wholeMessageCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        wholeMessageSelected_ = checked;
        update();
        emit wholeMessageSelectionToggled(this->post.id, checked);
    });

    reactionAffordance_ = new QPushButton(QString::fromUtf8("❤️"), this);
    reactionAffordance_->setFlat(true);
    reactionAffordance_->setFixedSize(28, 28);
    reactionAffordance_->setCursor(Qt::PointingHandCursor);
    reactionAffordance_->setToolTip(tr("Add reaction"));
    reactionAffordance_->setAccessibleName(tr("Add reaction"));
    QFont reactionFont = EmojiFont::applySystemEmojiFamily(reactionAffordance_->font());
    reactionFont.setPointSize(14);
    reactionAffordance_->setFont(reactionFont);
    reactionOpacity_ = new QGraphicsOpacityEffect(reactionAffordance_);
    reactionOpacity_->setOpacity(0.0);
    reactionAffordance_->setGraphicsEffect(reactionOpacity_);
    reactionAnimation_ = new QPropertyAnimation(reactionOpacity_, "opacity", this);
    reactionAnimation_->setDuration(140);
    reactionAffordance_->hide();
    connect(reactionAnimation_, &QPropertyAnimation::finished, this, [this] {
        if (reactionAffordance_ && !reactionAffordanceWanted_) {
            reactionAffordance_->hide();
        }
    });
    connect(reactionAffordance_, &QPushButton::clicked, this, [this] {
        showEmojiDialog([this](Emoji emoji) {
            backend_.addPostReaction(this->post.id, emoji.name);
        });
    });
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
	connect(messageContent, &MessageContentWidget::paletteRefreshCompleted,
	        this, &PostWidget::connectMessageLinks);
	messageContent->setMessage(displayMessage(post, post.message));
	connectMessageLinks();
	refreshPermalinkPreviews();
	ui->time->setText(getMessageTimeString(post.create_at));

    if (!post.isDeleted && KTalkMeetingWidget::supports(post)) {
        auto meeting = std::make_unique<KTalkMeetingWidget>(backend_, post, this);
        if (meeting->isValid()) {
            ktalkMeeting_ = std::move(meeting);
            ui->verticalLayout->insertWidget(
                messageIndex + 1, ktalkMeeting_.get(), 0, Qt::AlignLeft);
        }
    }

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
			post.user_id, [guard](const BackendUser* user) {
				if (guard && user) {
					guard->setAuthor(guard->backend_, user);
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
                AppNavigationService::instance(backend_).openPost(replyPostId);
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
                            if (guard) {
                                AppNavigationService::instance(guard->backend_)
                                    .openPost(replyPostId);
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
        const QString rootPostId = post.rootPost->id;
		connect(quoteFrame.get(), &PostQuoteFrame::postClicked, this,
                [this, rootPostId] {
            AppNavigationService::instance(backend_).openPost(rootPostId);
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
		reactions = std::make_unique<PostReactionList>(backend_, this);
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
		ui->verticalLayout->addWidget(poll.get());
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

void PostWidget::contextMenuEvent(QContextMenuEvent* event)
{
    if (!event) {
        return;
    }
    showPostContextMenu(event->globalPos());
    event->accept();
}

void PostWidget::paintEvent(QPaintEvent* event)
{
    QWidget::paintEvent(event);
    if (!wholeMessageSelected_
        && !property("_mmqt_contextMenuActive").toBool()) {
        return;
    }
    QColor selected = palette().color(QPalette::Highlight);
    selected.setAlpha(34);
    QPainter painter(this);
    painter.fillRect(rect(), selected);
}

void PostWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    positionReactionAffordance();
}

void PostWidget::setWholeMessageSelectionMode(bool enabled)
{
    if (wholeMessageSelectionMode_ == enabled) {
        return;
    }
    wholeMessageSelectionMode_ = enabled;
    if (wholeMessageCheck_) {
        wholeMessageCheck_->setVisible(enabled);
    }
    if (enabled) {
        clearTextSelection();
    }
    animateReactionAffordance(hovered_);
    updateGeometry();
    update();
}

void PostWidget::setWholeMessageSelected(bool selected)
{
    wholeMessageSelected_ = selected;
    if (wholeMessageCheck_) {
        const QSignalBlocker blocker(wholeMessageCheck_);
        wholeMessageCheck_->setChecked(selected);
    }
    update();
}

void PostWidget::setHovered(bool hovered)
{
    if (hovered_ == hovered) {
        return;
    }
    hovered_ = hovered;
    animateReactionAffordance(hovered_);
}

void PostWidget::clearTextSelection()
{
    if (messageContent) {
        messageContent->clearSelection();
    }
    if (ui && ui->authorName && ui->authorName->selectionStart() >= 0) {
        ui->authorName->setSelection(0, 0);
    }
}

void PostWidget::animateReactionAffordance(bool visible)
{
    visible = visible && !wholeMessageSelectionMode_ && !post.isDeleted;
    reactionAffordanceWanted_ = visible;
    if (!reactionAffordance_ || !reactionOpacity_ || !reactionAnimation_) {
        return;
    }
    reactionAnimation_->stop();
    if (visible) {
        positionReactionAffordance();
        reactionAffordance_->show();
        reactionAffordance_->raise();
    }
    reactionAnimation_->setStartValue(reactionOpacity_->opacity());
    reactionAnimation_->setEndValue(visible ? 1.0 : 0.0);
    reactionAnimation_->start();
}

void PostWidget::positionReactionAffordance()
{
    if (!reactionAffordance_) {
        return;
    }
    int x = 4;
    int y = std::max(4, height() - reactionAffordance_->height() - 6);
    if (threadSummary && threadSummary->isVisible()) {
        const QPoint threadTopLeft = threadSummary->mapTo(this, QPoint(0, 0));
        x = std::max(4, threadTopLeft.x() - reactionAffordance_->width() - 4);
        y = threadTopLeft.y()
            + (threadSummary->height() - reactionAffordance_->height()) / 2;
    }
    reactionAffordance_->move(x, std::max(2, y));
}

void PostWidget::showPostContextMenu(const QPoint& globalPos)
{
    if (post.isDeleted) {
        return;
    }

    setProperty("_mmqt_contextMenuActive", true);
    update();

    QMenu menu(this);
    const auto icon = [](const QString& path) { return IconUtils::symbolicIcon(path); };

    if (parentChatArea) {
        QAction* replyAction = menu.addAction(icon(QStringLiteral(":/icons/message-balloon")),
                                             tr("Reply"));
        connect(replyAction, &QAction::triggered, this, [this] {
            if (parentChatArea) {
                QuotedReplyController::instance(*parentChatArea).begin(post);
            }
        });
        menu.addSeparator();
    }

    if (post.isOwnPost()) {
        if (parentChatArea) {
            QAction* editAction = menu.addAction(icon(QStringLiteral(":/icons/edit")), tr("Edit"));
            connect(editAction, &QAction::triggered, this, [this] {
                parentChatArea->editPost(post);
            });
        }
        QAction* deleteAction = menu.addAction(icon(QStringLiteral(":/icons/trash")), tr("Delete"));
        connect(deleteAction, &QAction::triggered, this, [this] {
            backend_.deletePost(post.id);
        });
        menu.addSeparator();
    }

    if (!hoveredLink.isEmpty()) {
        QAction* copyLinkAction = menu.addAction(icon(QStringLiteral(":/icons/link")),
                                                tr("Copy link to clipboard"));
        connect(copyLinkAction, &QAction::triggered, this, [this] {
            QApplication::clipboard()->setText(hoveredLink);
        });
    }

    QAction* copyMessageLinkAction = menu.addAction(icon(QStringLiteral(":/icons/link")),
                                                    tr("Copy message link"));
    connect(copyMessageLinkAction, &QAction::triggered, this, [this] {
        const QString link = messagePermalink(post);
        if (!link.isEmpty()) {
            QApplication::clipboard()->setText(link);
        }
    });

    const QString selectedText = getSelectedText();
    if (!selectedText.isEmpty()) {
        QAction* copySelectedAction = menu.addAction(icon(QStringLiteral(":/icons/copy")),
                                                     tr("Copy selected text"));
        connect(copySelectedAction, &QAction::triggered, this, [selectedText] {
            QApplication::clipboard()->setText(selectedText);
        });
    }

    QAction* copyMessageAction = menu.addAction(icon(QStringLiteral(":/icons/copy")),
                                                tr("Copy post message"));
    connect(copyMessageAction, &QAction::triggered, this, [this] {
        QApplication::clipboard()->setText(formatForClipboardSelection(messageOnly));
    });

    QAction* unreadAction = menu.addAction(icon(QStringLiteral(":/icons/unread")),
                                           tr("Mark as unread"));
    connect(unreadAction, &QAction::triggered, this, [this] {
        emit markUnreadRequested(post.id);
    });

    QAction* saveAction = menu.addAction(icon(QStringLiteral(":/icons/bookmark")),
                                         tr("Save message"));
    connect(saveAction, &QAction::triggered, this, [this] {
        backend_.updateUserPreferences(BackendUserPreferences {
            QStringLiteral("flagged_post"), post.id, QStringLiteral("true")});
    });

    if (post.author) {
        menu.addSeparator();
        QAction* profileAction = menu.addAction(
            icon(QStringLiteral(":/icons/members")),
            tr("View %1's profile").arg(post.author->getDisplayName()));
        connect(profileAction, &QAction::triggered, this, [this] {
            if (!post.author) {
                return;
            }
            auto* dialog = new UserProfileDialog(backend_, *post.author, this);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->show();
        });
    }

    menu.exec(globalPos);
    setProperty("_mmqt_contextMenuActive", false);
    update();
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
	refreshPermalinkPreviews();

	if (post.poll) {
		clearMessageText();
		std::unique_ptr<PostPoll> newPoll =
			std::make_unique<PostPoll>(backend_, post, *post.poll, this);
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
        groupMentionIds = MentionGroupService::instance(backend_).mentionIds(teamId);
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
			AppNavigationService::instance(backend_).openUrl(url);
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
        auto* dialog = new UserProfileDialog(backend_, *user, this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
    };

    for (const auto& entry : backend_.getStorage().getAllUsers()) {
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
    UserProfileService::instance(backend_).searchUsers(
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

    auto& service = MentionGroupService::instance(backend_);
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
		reactions = std::make_unique<PostReactionList>(backend_, this);
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
		backend_.addPostReaction(post.id, emojiName);
	});
}

void PostWidget::addThreadButton()
{
	if (!threadSummary) {
		threadSummary = new ThreadSummaryWidget(backend_,
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
    if (!parentChatArea || post.id.isEmpty()) {
        return;
    }

    AppNavigationService::instance(backend_).openThread(
        parentChatArea->getChannel().id, post.id);
}

void PostWidget::markAsDeleted()
{
	post.isDeleted = true;
	quoteFrame.reset();
	quotedReplyPreview.reset();
	permalinkPreviews.clear();
	attachments.reset();
	reactions.reset();
    ktalkMeeting_.reset();
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
