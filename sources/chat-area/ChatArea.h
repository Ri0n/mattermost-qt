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

#include <QDate>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QTreeWidgetItem>
#include <QWidget>

#include "outgoing-post/OutgoingPostCreator.h"

namespace Ui {
class ChatArea;
}

class QDockWidget;

namespace Mattermost {

class AbstractPostSource;
class Backend;
class BackendChannel;
class BackendPost;
class BackendUser;
class ChannelItem;

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
	void handleUserTyping (const BackendUser& user);

	/** Scroll to a post through the logical post source, materializing it if known. */
	void goToPost (const BackendPost& post);
	void goToPost (const QString& postId);

	bool ensurePostVisible (const QString& postId);
	bool ensurePinnedPostVisible(const QString& postId,
	                             const QStringList& contextPostIds,
	                             bool reachedOldest,
	                             bool reachedNewest);

	/**
	 * Compatibility surface for semantic navigation callers. LongListWidget owns
	 * the actual durable anchor and suppresses layout-induced scroll changes.
	 */
	void lockNavigationToPost(const QString& postId, int quietPeriodMs = 2000);

	void onActivate ();
	void onDeactivate ();
	void onMainWindowActivate ();
	void onMove (QPoint pos);

	void requestExplicitReadAcknowledgement ();
private:
	void resizeEvent (QResizeEvent* event) override;
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
	void setupPostSource();

	ChatArea* parentArea;
	QString parentPostId;
	QString pendingPostId;
	AbstractPostSource* postSource = nullptr; // QObject child; owned by ChatArea

public:
	Ui::ChatArea* ui;
	Backend& backend;
	BackendChannel& channel;
	ChannelItem* treeItem;
	QString lastReadPostId;
	QPointer<QDockWidget> pinnedPostsDockWidget;
	void init();
	void deinit();

	uint32_t unreadMessagesCount;
	int texteditDefaultHeight;
	bool isThread;
	bool initialized;
	bool explicitReadPending = false;
	QMetaObject::Connection explicitReadPostsConnection;
	QSet<ChatArea*> threadsAreas;
	QString root_id;
	std::vector<QMetaObject::Connection> signalConnections;
};

} /* namespace Mattermost */
