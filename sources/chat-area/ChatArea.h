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

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

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
class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QEvent;
class QPushButton;
class QResizeEvent;
class QShowEvent;
class QTimer;

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
	void editPost(BackendPost& post);

	/** Scroll to a post through the logical post source, materializing it if known. */
	void goToPost (const BackendPost& post);
	void goToPost (const QString& postId);

	bool ensurePostVisible (const QString& postId);
	bool ensurePinnedPostVisible(const QString& postId,
	                             const QStringList& contextPostIds,
	                             bool reachedOldest,
	                             bool reachedNewest);

	/**
	 * Keep semantic navigation attached to a post ID while the source may replace
	 * an estimated logical slot with its authoritative index. Pixel anchoring
	 * remains exclusively inside LongListWidget.
	 */
	void lockNavigationToPost(const QString& postId, int quietPeriodMs = 2000);

	void onActivate ();
	void onDeactivate ();
	void onMainWindowActivate ();
	void onMove (QPoint pos);

	void requestExplicitReadAcknowledgement ();
private:
	void changeEvent(QEvent* event) override;
	bool eventFilter(QObject* watched, QEvent* event) override;
	void showEvent(QShowEvent* event) override;
	void resizeEvent (QResizeEvent* event) override;
	void dragEnterEvent (QDragEnterEvent* event) override;
	void dragMoveEvent (QDragMoveEvent* event) override;
	void dropEvent (QDropEvent* event) override;

	void setupComposerUi();
	void focusComposer();
	void beginMessageLoading();
	void endMessageLoading();
	void refreshActionIcons();
	void refreshActionIcon(QPushButton& button,
	                       const QString& resourcePath,
	                       const char* debugMarker,
	                       bool hovered);
	void setUserAvatar (const BackendUser& user);
	void refreshHeaderStatus();
	void moveOnListTop ();
	void setUnreadMessagesCount (uint32_t count);
	void updatePinnedPostsButton ();
	void updateThreadWindowTitle ();
	void markChannelViewedIfAtBottom ();
	void tryExplicitReadAcknowledgement ();
	void setupPostSource();
	void scheduleNewestPosition();
	void finishPendingNavigation();

	ChatArea* parentArea;
	QString parentPostId;
	QString pendingPostId;
	AbstractPostSource* postSource = nullptr; // QObject child; owned by ChatArea
	QWidget* loadingIndicator = nullptr;
	QTimer* loadingDelayTimer = nullptr;
	int pendingMessageLoads = 0;

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
	bool isThread;
	bool initialized;
	bool explicitReadPending = false;
	QMetaObject::Connection explicitReadPostsConnection;
	QSet<ChatArea*> threadsAreas;
	QString root_id;
	std::vector<QMetaObject::Connection> signalConnections;
};

} /* namespace Mattermost */
