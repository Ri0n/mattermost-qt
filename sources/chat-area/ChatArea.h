/**
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

#include <algorithm>
#include <cstdint>
#include <functional>
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

class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QEvent;
class QResizeEvent;
class QShowEvent;
class QStackedWidget;
class QTimer;
class QToolButton;

namespace Mattermost {

class AbstractPostSource;
class Backend;
class BackendChannel;
class BackendPost;
class BackendUser;
class ChannelItem;
class PostCollectionView;

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
    ChatArea* parentChatArea() const { return parentArea.data(); }
    /** Last semantic centre captured when an inactive channel view was detached. */
    QString storedNavigationBookmark() const { return storedViewportPostId; }
	void handleUserTyping (const BackendUser& user);
	void editPost(BackendPost& post);

	/**
	 * Explicit semantic navigation supersedes weak queued activation positioning
	 * (newest or an inactive-page bookmark). Call this synchronously as soon as
	 * an external jump selects this ChatArea, before the queued navigation itself
	 * runs.
	 */
	void preparePostNavigation () { ++viewportNavigationGeneration; }

	/** Explicitly navigate to the newest edge, superseding any older post target. */
	void goToNewest ()
	{
		++viewportNavigationGeneration;
		scheduleNewestPosition();
	}

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
	bool lockNavigationToPost(const QString& postId, int quietPeriodMs = 2000);

	/**
	 * Flash a semantic thread target only after its provisional index has been
	 * replaced or confirmed by an authoritative server window. onPresented runs
	 * only for the still-current navigation after that authoritative placement.
	 */
	void highlightPostWhenAuthoritative(const QString& postId,
	                                    std::function<void()> onPresented = {});

	void onActivate ();
	void onDeactivate ();
	void onMainWindowActivate ();
	void onMove (QPoint pos);

	/** Re-evaluate read progress from the current concrete timeline viewport. */
	void refreshReadState ();
private:
	void changeEvent(QEvent* event) override;
	void showEvent(QShowEvent* event) override;
	void resizeEvent (QResizeEvent* event) override;
	void dragEnterEvent (QDragEnterEvent* event) override;
	void dragMoveEvent (QDragMoveEvent* event) override;
	void dropEvent (QDropEvent* event) override;

	void setupHeaderUi();
	void refreshHeaderActionIcons();
	void updateUsersButton();
	void setupPinnedPostsView();
	void showPinnedPosts(bool show);
	void setupComposerUi();
	void focusComposer();
	void beginMessageLoading();
	void endMessageLoading();
	void setUserAvatar (const BackendUser& user);
	void moveOnListTop ();
	void setUnreadMessagesCount (uint32_t count);
	void updatePinnedPostsButton ();
	void updateThreadWindowTitle ();
	void setupPostSource();
	void scheduleNewestPosition();
	void scheduleStoredPosition();

	QPointer<ChatArea> parentArea;
	QString parentPostId;
	QString storedViewportPostId;
	std::uint64_t viewportNavigationGeneration = 0;
	AbstractPostSource* postSource = nullptr; // QObject child; owned by ChatArea
	QTimer* loadingDelayTimer = nullptr;
	QToolButton* threadFollowButton = nullptr;
	QStackedWidget* contentStack = nullptr;
	PostCollectionView* pinnedPostsView = nullptr;
	int pendingMessageLoads = 0;

public:
	Ui::ChatArea* ui;
	Backend& backend;
	BackendChannel& channel;
	ChannelItem* treeItem;
	QString lastReadPostId;
	void init();
	void deinit();

	uint32_t unreadMessagesCount;
	bool isThread;
	bool initialized;
	QSet<ChatArea*> threadsAreas;
	QString root_id;
	std::vector<QMetaObject::Connection> signalConnections;
};

} /* namespace Mattermost */
