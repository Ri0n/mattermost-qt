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

#pragma once

#include <QWidget>
#include <QDate>
#include <QPointer>
#include <QSet>
#include <QTreeWidgetItem>
#include <QScrollBar>

#include "outgoing-post/OutgoingPostCreator.h"

namespace Ui {
class ChatArea;
}

class QListWidgetItem;
class QVBoxLayout;
class QDockWidget;
class Qset;

namespace Mattermost {

class BackendFile;
class Backend;
class BackendChannel;
class BackendPost;
class BackendUser;
class ChannelItem;
struct ChannelNewPosts;
class OutgoingAttachmentList;
class QChatArea;
class ChatArea;
class ThreadTimelineController;
ThreadTimelineController* createThreadTimelineController(ChatArea& area);

class ChatArea: public QWidget {
	Q_OBJECT
public:
	explicit ChatArea (Backend& backend, BackendChannel& channel, ChannelItem* treeItem, QWidget *parent = nullptr, bool initialize = true);
	explicit ChatArea (Backend& backend, BackendChannel& channel, QString rootId, ChatArea* parentArea); //for thread window
	~ChatArea();
public:
	Ui::ChatArea* getUi ();
	Backend& getBackend ();
	BackendChannel& getChannel ();
	void appendChannelPost (BackendPost& post);
	void fillChannelPosts (const ChannelNewPosts& newPosts);
	void handleUserTyping (const BackendUser& user);

	/**
	 * Scroll to given post. If it is not loaded yet, remember the target and
	 * complete the navigation when the post arrives.
	 */
	void goToPost (const BackendPost& post);
	void goToPost (const QString& postId);

	/**
	 * Ensure a cached root post has a visible row in the normal channel view.
	 * Context navigation can cache the target before its surrounding chunks are
	 * rendered; this bridges backend identity with the current materialized UI.
	 */
	bool ensurePostVisible (const QString& postId);

	/**
	 * Called when the chat area is being selected from the channels menu (so that it's contents is shown)
	 */
	void onActivate ();

	/**
	 * Called when the chat area is being unselected from the channels menu
	 * (so that other chatArea is being activated)
	 */
	void onDeactivate ();

	/**
	 * Called when the main Mattermost window is being activated (gains focus).
	 * Called only if the chat area is the currently active one (so that it's contents is visible)
	 */
	void onMainWindowActivate ();

	void onMove (QPoint pos);

	// A user explicitly chose this conversation. Acknowledge it as read only
	// after the newest channel content has actually been materialized in the UI.
	void requestExplicitReadAcknowledgement ();
private:
	void resizeEvent (QResizeEvent* event)		override;
	void dragEnterEvent (QDragEnterEvent* event) override;
	void dragMoveEvent (QDragMoveEvent* event) override;
	void dropEvent (QDropEvent* event) override;

	void setUserAvatar (const BackendUser& user);
	void moveOnListTop ();
	void setUnreadMessagesCount (uint32_t count);
	void setTextEditWidgetHeight (int height);
	void updatePinnedPostsButton ();
	void markChannelViewedIfAtBottom ();
	void tryExplicitReadAcknowledgement ();
	
	ChatArea*					parentArea;
	QString						parentPostId;
	QString						pendingPostId;

public:
	Ui::ChatArea 					*ui;
	Backend& 						backend;
	BackendChannel& 				channel;
	ChannelItem* 					treeItem;
	QString 						lastReadPostId;
	QPointer<QDockWidget>			pinnedPostsDockWidget;
	void						init();
	void						deinit();


	uint32_t						unreadMessagesCount;
	int 							texteditDefaultHeight;
	QDate							lastPostDate;
	bool							gettingOlderPosts;
	bool							areaIsFilled;
	//thread chat window
	bool							isThread;
	bool							postsRetrieved;
	//ChatArea can be created without initializing (useful for rarely used channels)
	volatile bool						initialized;
	bool							explicitReadPending = false;
	QMetaObject::Connection			explicitReadPostsConnection;
	QSet<ChatArea*> 					threadsAreas;
	QString							root_id;
	std::vector<QMetaObject::Connection> 		signalConnections;

	// Declared last on purpose: isThread/root_id/ui/backend/channel are already
	// initialized when this factory runs. The controller itself defers UI access
	// until the next event-loop turn, after the constructor body has completed.
	ThreadTimelineController*		threadTimelineController = createThreadTimelineController(*this);
};

} /* namespace Mattermost */
