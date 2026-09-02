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

#include "ChatArea.h"

#include <QDockWidget>
#include <QPointer>
#include <QPushButton>

#include "PinnedPostsList.h"
#include "backend/Backend.h"
#include "backend/SidebarService.h"
#include "backend/ThreadFollowService.h"
#include "channel-tree/ChannelItem.h"
#include "channel-tree/ChannelItemWidget.h"
#include "channel-tree-dialogs/ViewChannelMembersListDialog.h"
#include "log.h"
#include "post/PostWidget.h"
#include "ui_ChatArea.h"

namespace Mattermost {

static const QIcon& getUserButtonIcon ()
{
	static QIcon icon (QString::fromUtf8(":/img/user-icon.png"));
	return icon;
}

ChatArea::ChatArea (Backend& backend, BackendChannel& channel, ChannelItem* treeItem, QWidget *parent, bool initialize)
:QWidget(parent)
,parentArea(nullptr)
,ui(new Ui::ChatArea)
,backend (backend)
,channel (channel)
,treeItem (treeItem)
,pinnedPostsDockWidget (nullptr)
,unreadMessagesCount (0)
,texteditDefaultHeight (70)
,gettingOlderPosts (false)
,areaIsFilled(false)
,isThread(false)
,postsRetrieved(false)
,initialized(false) {
	//accept drag&drop attachments
	setAcceptDrops(true);

	ui->setupUi(this);

	ui->usersButton->setIcon(getUserButtonIcon());

	ui->outgoingPostCreator->init (backend, channel, *ui->outgoingPostPanel, *ui->listWidget, ui->footerLayout);
	ui->listWidget->backend = &backend;

	ui->titleLabel->setText (channel.display_name);
	ui->statusLabel->setText (channel.getChannelDescription ());

	setTextEditWidgetHeight (texteditDefaultHeight);

	const BackendUser* user = backend.getStorage().getUserById (channel.name);

	if (user) {

		connect (user, &BackendUser::onAvatarChanged, [this, user] {
			setUserAvatar (*user);
		});

		if (!user->avatar.isNull()) {
			setUserAvatar (*user);
		} else {
			backend.retrieveUserAvatar(user->id);
		}

		connect (user, &BackendUser::onStatusChanged, [this, user] {
			ui->statusLabel->setText (user->status);
		});

		if (ui->statusLabel->text().isEmpty()) {
			ui->statusLabel->setText (user->status);
		}

	} else {
		ui->userAvatar->clear();
		ui->userAvatar->hide();
	}

	if (!user) {
		backend.retrieveChannelMembers (this->channel, [this] {
			ui->usersButton->show ();
			ui->usersButton->setText (QString::number (this->channel.members.size()) + " members");
		});
	}

	// ChatArea instances are lazy now, so pinned posts must be loaded when the
	// corresponding channel is first materialized instead of relying on the old
	// eager startup path. The response may arrive before init(); init() also
	// samples channel.pinnedPosts, so neither ordering loses the button state.
	backend.retrieveChannelPinnedPosts(channel);

	if (initialize)
		init();
}

void ChatArea::init() {
	if (initialized)
		return;

	const bool sparseTimeline = channelTimelineController != nullptr;

	//save all lambda slot connections, they cannot be disconnected without this

	signalConnections.push_back( connect (&channel, &BackendChannel::onViewed,this, [this] {
		LOG_DEBUG ("Channel viewed: " << this->channel.display_name);
		setUnreadMessagesCount (0);
		ui->listWidget->removeNewMessagesSeparatorAfterTimeout (1000);
	})
	);

	signalConnections.push_back( connect (&channel, &BackendChannel::onUpdated,this, [this] {
		ui->titleLabel->setText (this->channel.display_name);
		this->treeItem->setLabel (this->channel.display_name);
		ui->statusLabel->setText (this->channel.getChannelDescription ());
	})
	);

	// With sparse history the controller is the sole bulk materializer. Keeping
	// this legacy connection would make every REST page render twice: once here
	// and once when the sparse model rebuilds the viewport.
	if (!sparseTimeline) {
		connect (&channel, &BackendChannel::onNewPosts, this, &ChatArea::fillChannelPosts);
	}

	signalConnections.push_back( connect (&channel, &BackendChannel::onPinnedPostsReceived,this, [this] () {
		updatePinnedPostsButton();
	})
	);

	connect (&channel, &BackendChannel::onNewPost, this, &ChatArea::appendChannelPost);

	// Only genuine user scrolling is allowed to advance the read position after
	// the initial explicit channel selection. Layout-driven scrollbar movement
	// (images, Markdown reflow, resize, anchor restoration) never emits this
	// signal, so it cannot accidentally mark messages as read.
	signalConnections.push_back(connect(ui->listWidget, &PostsListWidget::userViewportChanged,
	                                  this, [this](bool atBottom) {
		if (atBottom) {
			markChannelViewedIfAtBottom();
		}
	}));

	//let the post creator know that the last sent / edited post has appeared so that the input box can be cleared
	connect (&channel, &BackendChannel::onNewPost, ui->outgoingPostCreator, &OutgoingPostCreator::onPostReceived);
	connect (&channel, &BackendChannel::onPostEdited, ui->outgoingPostCreator, &OutgoingPostCreator::onPostReceived);

	connect (&channel, &BackendChannel::onUserTyping, this, &ChatArea::handleUserTyping);

	signalConnections.push_back( connect (&channel, &BackendChannel::onPostEdited, this,[this] (BackendPost& post) {
		PostWidget* postWidget = ui->listWidget->findPost (post.id);
		if (postWidget) {
			postWidget->setEdited (post.message);
			if (post.has_thread && !isThread)
				postWidget->addThreadButton();
			ui->listWidget->adjustSize();
		}
	})
	);

	signalConnections.push_back( connect (&channel, &BackendChannel::onPostReactionUpdated,this, [this] (BackendPost& post) {
		PostWidget* postWidget = ui->listWidget->findPost (post.id);

		if (postWidget) {
			postWidget->updateReactions ();
			ui->listWidget->adjustSize();
		}
	})
	);

	signalConnections.push_back( connect (&channel, &BackendChannel::onPostDeleted,this, [this] (const QString& postId) {
		PostWidget* postWidget = ui->listWidget->findPost (postId);
		if (postWidget) {
			postWidget->markAsDeleted ();
			ui->listWidget->adjustSize();
		}
	})
	);

	signalConnections.push_back( connect (&channel, &BackendChannel::onUserAdded, this, [this] (const BackendUser&) {
		ui->usersButton->setText (QString::number (this->channel.members.size()) + " members");
	})
	);

	signalConnections.push_back( connect (&channel, &BackendChannel::onUserRemoved, this, [this] (const BackendUser&) {
		ui->usersButton->setText (QString::number (this->channel.members.size()) + " members");
	})
	);
	//initiate editing of post, when edit is selected from the context menu
	connect (ui->listWidget, &PostsListWidget::postEditInitiated, ui->outgoingPostCreator, &OutgoingPostCreator::postEditInitiated);

	connect (ui->outgoingPostCreator, &OutgoingPostCreator::postEditFinished, ui->listWidget, &PostsListWidget::postEditFinished);


	signalConnections.push_back( connect (ui->splitter, &QSplitter::splitterMoved,this, [this] {
		texteditDefaultHeight = ui->splitter->sizes()[1];
	})
	);

	signalConnections.push_back( connect (ui->outgoingPostCreator, &OutgoingPostCreator::heightChanged, this, [this] (int height) {

		if (height < texteditDefaultHeight) {
			height = texteditDefaultHeight;
		}

		setTextEditWidgetHeight (height);
	})
	);

	if (!sparseTimeline) {
		// Legacy fallback for builds/views that do not install a sparse controller.
		signalConnections.push_back( connect (ui->loadOldPosts, &QPushButton::clicked, this,[this] {
			if (!gettingOlderPosts && !channel.posts.empty()) {
				gettingOlderPosts = true;
				backend.retrieveChannelOlderPosts (channel, 140);
			}
		})
		);
	}

	signalConnections.push_back( connect (ui->usersButton, &QPushButton::clicked, this,[this] {
		ViewChannelMembersListDialog* dialog = new ViewChannelMembersListDialog (this->backend, this->channel, this);
		dialog->show ();
	})
	);

	signalConnections.push_back( connect (ui->pinnedPostsButton, &QPushButton::clicked, this,[this] {

		if (pinnedPostsDockWidget) {
			delete (pinnedPostsDockWidget);
			pinnedPostsDockWidget = nullptr;
			return;
		}

		pinnedPostsDockWidget = new QDockWidget (this);
		pinnedPostsDockWidget->setFloating(true);
		pinnedPostsDockWidget->setFeatures(QDockWidget::DockWidgetMovable);
		auto pinnedPostsList = new PinnedPostsList (this);
		pinnedPostsDockWidget->setWidget (pinnedPostsList);

		for (auto& post: this->channel.pinnedPosts) {
			pinnedPostsList->addPost (new PostWidget (this->backend, post, ui->listWidget, this, nullptr));
		}

		//disable title bar
		pinnedPostsDockWidget->setTitleBarWidget(new QWidget());

		pinnedPostsDockWidget->move (mapToGlobal(ui->pinnedPostsButton->pos()) + QPoint (0,40));
		pinnedPostsDockWidget->setFixedWidth (geometry().size().width() - ui->pinnedPostsButton->pos().x() - 30);
		pinnedPostsDockWidget->setFixedHeight (300);

		ui->headerLayout->addWidget(pinnedPostsDockWidget);

	})
	);

	if (!sparseTimeline) {
		/*
		 * Legacy initial population. Sparse timelines intentionally bypass this
		 * path so opening a channel creates exactly one 30-post widget window.
		 */
		int elapsedDaysSinceLastNewPost = INT32_MAX;
		QDate currentDate = QDateTime::currentDateTime().date();
		if (postsRetrieved){
			int insertPos = 0;
			for(auto& post: channel.posts ) {
				if (post.root_id.isEmpty()){
					int elapsedDaysSinceThisNewPost = post.getCreationTime ().date().daysTo (currentDate);
					if (elapsedDaysSinceThisNewPost != elapsedDaysSinceLastNewPost) {
						elapsedDaysSinceLastNewPost = elapsedDaysSinceThisNewPost;
						ui->listWidget->addDaySeparator (insertPos, elapsedDaysSinceThisNewPost);
						++insertPos;
					}

					ui->listWidget->insertPost (insertPos, new PostWidget (backend, post, ui->listWidget, this, nullptr));
					++insertPos;

					if (post.id == lastReadPostId) {
						ui->listWidget->addNewMessagesSeparator ();
						++insertPos;
					}
				}
			}
			ui->listWidget->updateGeometry();
			ui->listWidget->resizeToBottom();
		} else {
			backend.retrieveChannelPosts (channel, 0, 100);
			postsRetrieved = true;
		}
	} else {
		// Prevent any later legacy fallback from treating the sparse first page as
		// an uninitialized channel and issuing another initial REST request.
		postsRetrieved = true;
	}

	// Pinned posts may have been retrieved before this lazy ChatArea existed.
	// Never rely solely on the edge-triggered onPinnedPostsReceived signal.
	updatePinnedPostsButton();

	//hide the users button. It will be shown when the channel members list is retrieved
	ui->usersButton->hide();

	initialized = true;

	if (!sparseTimeline) {
		// Legacy fallback only. ChannelTimelineController owns this trigger for the
		// normal sparse path.
		signalConnections.push_back( connect (ui->listWidget, &PostsListWidget::scrolledToTop, this, [this] {
			if (!gettingOlderPosts && !channel.posts.empty()) {
				gettingOlderPosts = true;
				backend.retrieveChannelOlderPosts (channel, 40);
			}
		})
		);
	}

	if (!pendingPostId.isEmpty()) {
		goToPost(pendingPostId);
	}
}

void ChatArea::deinit() {
	if (!initialized)
		return;
	for (auto& it: signalConnections) {
		disconnect(it);
	}
	disconnect(&channel, &BackendChannel::onNewPost, this, &ChatArea::appendChannelPost);
	signalConnections.clear();
	ui->listWidget->clear();
	lastReadPostId.clear();
	initialized = false;
}

ChatArea::ChatArea (Backend& backend, BackendChannel& channel, QString rootId, ChatArea* parentArea)
:QWidget(nullptr)
,parentArea(parentArea)
,parentPostId(rootId)
,ui(new Ui::ChatArea)
,backend (backend)
,channel (channel)
,treeItem (nullptr)
,pinnedPostsDockWidget (nullptr)
,unreadMessagesCount (0)
,texteditDefaultHeight (70)
,gettingOlderPosts (false)
,areaIsFilled(false)
,isThread(true)
,postsRetrieved(true)
,initialized(true)
,root_id(rootId)
{
	//accept drag&drop attachments
	setAttribute (Qt::WA_DeleteOnClose);
	setAcceptDrops(true);

	ui->setupUi(this);

	ui->outgoingPostCreator->init (backend, channel, *ui->outgoingPostPanel, *ui->listWidget, ui->footerLayout);
	ui->listWidget->backend = &backend;

	ui->titleLabel->setText (channel.display_name);
	ui->statusLabel->setText (channel.getChannelDescription ());

	ui->outgoingPostCreator->setRootId(rootId);

	setTextEditWidgetHeight (texteditDefaultHeight);

	ui->userAvatar->hide();

	// ThreadTimelineController owns thread history from the first paint. Do not
	// construct an arbitrary cached subset only to destroy it on the next event
	// loop turn. Keep the legacy path solely as a fallback if no controller was
	// installed.
	if (!threadTimelineController) {
		int insertPos = 0;
		BackendPost* lastRootPost = nullptr;
		QDate currentDate = QDateTime::currentDateTime().date();
		int elapsedDaysSinceLastNewPost = INT32_MAX;
		for(auto& post: channel.posts ) {
			if (post.root_id == rootId || post.id == rootId){
				int elapsedDaysSinceThisNewPost = post.getCreationTime ().date().daysTo (currentDate);
				if (elapsedDaysSinceThisNewPost != elapsedDaysSinceLastNewPost) {
					elapsedDaysSinceLastNewPost = elapsedDaysSinceThisNewPost;
					ui->listWidget->addDaySeparator (insertPos, elapsedDaysSinceThisNewPost);
					++insertPos;
				}

				ui->listWidget->insertPost (insertPos, new PostWidget (backend, post, ui->listWidget, this, lastRootPost));
				lastRootPost = post.rootPost;
				++insertPos;

				if (post.id == lastReadPostId) {
					ui->listWidget->addNewMessagesSeparator ();
					++insertPos;
				}
			}
		}

		connect (&channel, &BackendChannel::onNewPosts, this, &ChatArea::fillChannelPosts);
		connect (&channel, &BackendChannel::onNewPost, this, &ChatArea::appendChannelPost);
	}

	//let the post creator know that the last sent / edited post has appeared so that the input box can be cleared
	connect (&channel, &BackendChannel::onNewPost, ui->outgoingPostCreator, &OutgoingPostCreator::onPostReceived);
	connect (&channel, &BackendChannel::onPostEdited,  ui->outgoingPostCreator, &OutgoingPostCreator::onPostReceived);

	connect (&channel, &BackendChannel::onUserTyping, this, &ChatArea::handleUserTyping);

	connect (&channel, &BackendChannel::onPostEdited, this, [this] (BackendPost& post) {
		PostWidget* postWidget = ui->listWidget->findPost (post.id);
		if (postWidget) {
			postWidget->setEdited (post.message);
			ui->listWidget->adjustSize();
		}
	});

	connect (&channel, &BackendChannel::onPostReactionUpdated, this, [this] (BackendPost& post) {

		PostWidget* postWidget = ui->listWidget->findPost (post.id);

		if (postWidget) {
			postWidget->updateReactions ();
			ui->listWidget->adjustSize();
		}
	});

	connect (&channel, &BackendChannel::onPostDeleted, this, [this] (const QString& postId) {
		PostWidget* postWidget = ui->listWidget->findPost (postId);

		if (postWidget) {
			postWidget->markAsDeleted ();
			ui->listWidget->adjustSize();
		}
	});

	//initiate editing of post, when edit is selected from the context menu
	connect (ui->listWidget, &PostsListWidget::postEditInitiated, ui->outgoingPostCreator, &OutgoingPostCreator::postEditInitiated);

	connect (ui->outgoingPostCreator, &OutgoingPostCreator::postEditFinished, ui->listWidget, &PostsListWidget::postEditFinished);


	connect (ui->splitter, &QSplitter::splitterMoved, this, [this] {
		texteditDefaultHeight = ui->splitter->sizes()[1];
	});

	connect (ui->outgoingPostCreator, &OutgoingPostCreator::heightChanged, this, [this] (int height) {

		if (height < texteditDefaultHeight) {
			height = texteditDefaultHeight;
		}

		setTextEditWidgetHeight (height);
	});

	// Thread follow state is a per-user Mattermost resource. Query it lazily when
	// the thread window opens and use the server's official PUT/DELETE endpoint
	// instead of keeping another local preference.
	if (channel.team && !rootId.isEmpty()) {
		const QString teamId = channel.team->id;
		auto* followButton = new QPushButton(tr("Follow"), this);
		followButton->setEnabled(false);
		followButton->setProperty("following", false);
		ui->propertieslLayout->insertWidget(0, followButton);

		QPointer<ChatArea> areaGuard(this);
		QPointer<QPushButton> buttonGuard(followButton);
		auto setButtonState = [areaGuard, buttonGuard](bool following) {
			if (!areaGuard || !buttonGuard) {
				return;
			}
			buttonGuard->setProperty("following", following);
			buttonGuard->setText(following ? areaGuard->tr("Unfollow")
			                              : areaGuard->tr("Follow"));
			buttonGuard->setEnabled(true);
		};

		auto& followService = ThreadFollowService::instance(backend);
		connect(&followService, &ThreadFollowService::followingChanged, followButton,
		        [teamId, rootId, setButtonState](const QString& changedTeamId,
		                                         const QString& changedThreadId,
		                                         bool following) {
			if (changedTeamId == teamId && changedThreadId == rootId) {
				setButtonState(following);
			}
		});

		followService.queryFollowing(teamId, rootId, setButtonState);
		connect(followButton, &QPushButton::clicked, this,
		        [this, teamId, rootId, buttonGuard, setButtonState] {
			if (!buttonGuard) {
				return;
			}
			const bool following = buttonGuard->property("following").toBool();
			const bool desired = !following;
			buttonGuard->setEnabled(false);
			ThreadFollowService::instance(this->backend).setFollowing(
				teamId, rootId, desired,
				[buttonGuard, following, desired, setButtonState](bool success) {
					if (!buttonGuard) {
						return;
					}
					setButtonState(success ? desired : following);
				});
		});
	}

	//threads do not expose the channel-level pinned-posts panel
	ui->pinnedPostsButton->hide();

	//hide the users button. It will be shown when the channel members list is retrieved
	ui->usersButton->hide();
}

ChatArea::~ChatArea()
{
	if (isThread)
		parentArea->threadsAreas.remove(this);
	delete ui;
}

void ChatArea::setUserAvatar (const BackendUser& user)
{
	ui->userAvatar->setPixmap (user.avatar);

	if (channel.type == BackendChannel::directChannel && !isThread)
		treeItem->setIcon (QIcon(user.avatar));
}

Ui::ChatArea* ChatArea::getUi ()
{
	return ui;
}

Backend& ChatArea::getBackend ()
{
	return backend;
}

BackendChannel& ChatArea::getChannel ()
{
	return channel;
}

void ChatArea::updatePinnedPostsButton ()
{
	if (isThread || channel.pinnedPosts.empty()) {
		ui->pinnedPostsButton->hide();
		return;
	}

	const auto pinnedPostCount = channel.pinnedPosts.size();
	ui->pinnedPostsButton->setText(
		QString::number(static_cast<qulonglong>(pinnedPostCount))
		+ (pinnedPostCount == 1 ? QStringLiteral(" pinned post")
		                        : QStringLiteral(" pinned posts")));
	ui->pinnedPostsButton->show();
}

void ChatArea::markChannelViewedIfAtBottom ()
{
	if (isThread || !initialized || !ui->listWidget->isAtBottom()) {
		return;
	}
	setUnreadMessagesCount(0);
	SidebarService::instance(backend).markChannelViewedLocally(channel);
	backend.markChannelAsViewed(channel);
}

void ChatArea::fillChannelPosts (const ChannelNewPosts& newPosts)
{
	QDate currentDate = QDateTime::currentDateTime().date();
	int insertPos = 0;
	int startPos = 0;
	int postSeq = 0;

	//elapsed days since the last post that was added from the new posts packet
	int elapsedDaysSinceLastNewPost = INT32_MAX;

	//elapsed days since the oldest post that was available before retrieving older posts
	int elapsedDaysSinceFirstExistingPost = INT32_MAX;

	//save the first post (before insertion), so that the list will be scrolled to it after the insertion
	QListWidgetItem* widgetToScrollTo = nullptr;
	QListWidgetItem* daySeparatorOnTop = nullptr;

	if (gettingOlderPosts) {

		widgetToScrollTo = ui->listWidget->item(0);
		uint32_t firstPostIndex = 0;

		if (widgetToScrollTo && widgetToScrollTo->data(Qt::UserRole) != ItemType::post) {
			daySeparatorOnTop = widgetToScrollTo;
			firstPostIndex = 1;
		}

		QListWidgetItem* firstPostItem = ui->listWidget->item(firstPostIndex);
		PostWidget* firstPostWidget = PostsListWidget::isPostItem(firstPostItem)
			? static_cast<PostWidget*> (ui->listWidget->itemWidget (firstPostItem))
			: nullptr;
		if (firstPostWidget)
			elapsedDaysSinceFirstExistingPost = firstPostWidget->post.getCreationTime().date().daysTo(currentDate);
	}

	BackendPost* lastRootPost = nullptr;

	for (const ChannelNewPostsChunk& chunk: newPosts.postsToAdd) {

		if (!chunk.previousPostId.isEmpty()) {
			qDebug() << "Add posts after" << chunk.previousPostId;
		}

		insertPos = ui->listWidget->findPostByIndex (chunk.previousPostId, startPos);
		++insertPos;
		startPos = insertPos;

		for (auto& post: chunk.postsToAdd) {

			if(post->hidden && !isThread)
				continue;

			if(post->root_id != root_id)
				continue;

			if (!chunk.previousPostId.isEmpty()) {
				qDebug() << "\tAdd post " << post->id;
			}

			int elapsedDaysSinceThisNewPost = post->getCreationTime ().date().daysTo (currentDate);

			if (elapsedDaysSinceThisNewPost != elapsedDaysSinceLastNewPost) {

				elapsedDaysSinceLastNewPost = elapsedDaysSinceThisNewPost;
				ui->listWidget->addDaySeparator (insertPos, elapsedDaysSinceThisNewPost);
				++insertPos;
			}

			ui->listWidget->insertPost (insertPos, new PostWidget (backend, *post, ui->listWidget, this, lastRootPost));
			lastRootPost = post->rootPost;
			++insertPos;
			++postSeq;

			if (post->id == lastReadPostId) {
				ui->listWidget->addNewMessagesSeparator ();
				++insertPos;
				++unreadMessagesCount;
			}
		}
	}

	if (postSeq > 20)
		areaIsFilled = true;	// all loaded posts may be hidden thread posts, allow load on scroll

	if (!gettingOlderPosts && !newPosts.postsToAdd.empty()) {
		lastPostDate = newPosts.postsToAdd.back().postsToAdd.back()->getCreationTime ().date();
	}

	gettingOlderPosts = false;

	if (elapsedDaysSinceLastNewPost == elapsedDaysSinceFirstExistingPost && daySeparatorOnTop) {
		delete (daySeparatorOnTop);
		daySeparatorOnTop = nullptr;
		qDebug () << "Delete day separator";
	}
	ui->listWidget->scrollToBottom ();
	if (!isThread)
		setUnreadMessagesCount (unreadMessagesCount);

	if (!pendingPostId.isEmpty()) {
		goToPost(pendingPostId);
	}
}

void ChatArea::appendChannelPost (BackendPost& post)
{
	if(post.hidden && !isThread)
		return;

	if(post.root_id != root_id)	//filter posts from other threads. posts without thread have empty root_id, so the non-thread window
		return;

	qDebug() << "appendChannelPost root_id" << post.root_id << post.hidden;
	QDate currentDate = QDateTime::currentDateTime().date();

	if (lastPostDate.daysTo (currentDate) >= 1) {
		ui->listWidget->addDaySeparator (0);
		QDateTime postTime = QDateTime::fromMSecsSinceEpoch (post.create_at);
		lastPostDate = postTime.date();
	}

	//thread window is not in the tree
	bool chatAreaHasFocus = isThread ? isActiveWindow () : treeItem->isSelected() && isActiveWindow ();

	if (!chatAreaHasFocus) {
		ui->listWidget->addNewMessagesSeparator ();
	}

	ui->listWidget->insertPost (new PostWidget (backend, post, ui->listWidget, this, nullptr));

	ui->listWidget->adjustSize();
	ui->listWidget->scrollToBottom();

	if (!pendingPostId.isEmpty()) {
		goToPost(pendingPostId);
	}

	//do not add unread messages count if the Chat Area is on focus
	if (chatAreaHasFocus) {
		return;
	}

	if (!isThread){
		++unreadMessagesCount;
		setUnreadMessagesCount (unreadMessagesCount);
	}
}

void ChatArea::handleUserTyping (const BackendUser& user)
{
	LOG_DEBUG ("Channel " << channel.display_name << ": " << user.getDisplayName() << " is typing");
}

void ChatArea::onActivate ()
{
	backend.setCurrentChannel (channel);
	init();
}

void ChatArea::onDeactivate ()
{
	if (pinnedPostsDockWidget) {
		delete pinnedPostsDockWidget;
		pinnedPostsDockWidget = nullptr;
	}
	deinit();
}

void ChatArea::onMainWindowActivate ()
{
	// Merely restoring/focusing the application is not a reading gesture. In
	// particular, automatic viewport restoration must never clear unread state.
}

void ChatArea::onMove (QPoint)
{
	if (!pinnedPostsDockWidget) {
		return;
	}

	pinnedPostsDockWidget->move (mapToGlobal(ui->pinnedPostsButton->pos()) + QPoint (0,40));
}

void ChatArea::moveOnListTop ()
{
	QTreeWidgetItem* parent = treeItem->parent();
	QTreeWidget* tree = treeItem->treeWidget();

	//item already on top, nothing to do
	if (parent->indexOfChild (treeItem) == 0) {
		return;
	}

	bool isCurrent = (tree->currentItem() == treeItem);

	ChannelItemWidget* thisItemWidget = static_cast<ChannelItemWidget*> (tree->itemWidget(treeItem, 0));

	ChannelItemWidget* newItemWidget = new ChannelItemWidget (thisItemWidget->parentWidget());
	newItemWidget->setLabel (channel.display_name);

	if (!thisItemWidget->getPixmap().isNull()) {
		newItemWidget->setIcon (QIcon(thisItemWidget->getPixmap()));
	}

	//block signals, so that itemActivated is not called

	tree->blockSignals (true);
	QTreeWidgetItem* child = parent->takeChild (parent->indexOfChild(treeItem));
	parent->insertChild(0, child);
	tree->blockSignals (false);

	if (child != treeItem) {
		exit (1);
	}

	tree->setItemWidget (child, 0, newItemWidget);
	treeItem->setWidget (newItemWidget);

	if (isCurrent) {
		tree->setCurrentItem (child);
	}
}

void ChatArea::setUnreadMessagesCount (uint32_t count)
{
	unreadMessagesCount = count;

	if (!treeItem) {
		return;
	}

	if (count == 0) {
		treeItem->setText(1, "");
	} else {
		treeItem->setText(1, QString::number(count));
	}
}

void ChatArea::resizeEvent (QResizeEvent* event)
{
	if (!initialized) {
		QWidget::resizeEvent(event);
		return;
	}
	// PostsListWidget distinguishes this layout-driven resize from user scroll
	// and restores its durable message anchor after the geometry settles.
	ui->listWidget->resizeToBottom();
	QWidget::resizeEvent (event);
}

void ChatArea::dragEnterEvent (QDragEnterEvent* event)
{
	ui->outgoingPostCreator->onDragEnterEvent (event);
}

void ChatArea::dragMoveEvent (QDragMoveEvent* event)
{
	ui->outgoingPostCreator->onDragMoveEvent (event);
}

void ChatArea::dropEvent (QDropEvent* event)
{
	ui->outgoingPostCreator->onDropEvent (event);
}

void ChatArea::goToPost (const BackendPost& post)
{
	goToPost(post.id);
}

void ChatArea::goToPost (const QString& postId)
{
	if (postId.isEmpty()) {
		return;
	}

	const int pos = ui->listWidget->findPostByIndex (postId, 0);
	if (pos < 0) {
		pendingPostId = postId;
		return;
	}

	pendingPostId.clear();
	ui->listWidget->scrollToItem(ui->listWidget->item(pos), QAbstractItemView::PositionAtTop);
	ui->listWidget->highlightPost(postId);
}

void ChatArea::setTextEditWidgetHeight (int height)
{
	//set the size of the text input area only. The chat area will take the whole remaining part, because it has higher stretch factor
	ui->splitter->setSizes({1, height});
}

} /* namespace Mattermost */