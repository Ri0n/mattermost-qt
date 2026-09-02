/**
 * @file PostsListWidget.h
 * @brief
 * @author Lyubomir Filipov
 * @date Dec 27, 2021
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

#pragma once

#include <QTimer>
#include "ResizableListWidget.h"
#include "post/PostWidget.h"

namespace Mattermost {

class PostWidget;

namespace ItemType {
enum id {
	post,
	separator,
};
}

namespace ItemRole {
enum id {
	postId = Qt::UserRole + 1,
};
}

class PostsListWidget: public ResizableListWidget {
	Q_OBJECT
public:
	explicit PostsListWidget (QWidget* parent);
	~PostsListWidget ();
public:
	void insertPost (int position, PostWidget* postWidget);
	void insertPost (PostWidget* postWidget);
	PostWidget* findPost (const QString& postId);
	int findPostByIndex (const QString& postId, int startIndex);

	// QListWidget::clear(), scrollToItem() and scrollToBottom() are not virtual.
	// ChatArea uses the concrete PostsListWidget type, so hiding them here lets
	// us distinguish explicit navigation from automatic layout-driven scrolling.
	void clear();
	void scrollToItem(const QListWidgetItem* item,
	                  QAbstractItemView::ScrollHint hint = QAbstractItemView::EnsureVisible);
	void scrollToBottom();

	static bool isPostItem (const QListWidgetItem* item)
	{
		if (!item) {
			return false;
		}

		const QVariant itemType = item->data(Qt::UserRole);
		return itemType.isValid() && itemType.toInt() == ItemType::post;
	}

	void scrollToUnreadPostsOrBottom ();
	void addDaySeparator (int daysAgo);
	void addDaySeparator (int insertPos, int daysAgo);
	void addNewMessagesSeparator ();
	void removeNewMessagesSeparator ();
	void removeNewMessagesSeparatorAfterTimeout (int timeoutMs);
	QListWidgetItem* getLastOwnPost () const;
	void initiatePostEdit (QListWidgetItem& postItem);
	void postEditFinished ();

	/**
	 * Preserve the last user-selected viewport across a resize. Kept for the
	 * existing ChatArea call site; it no longer decides based on the current
	 * scrollbar value, because that value may already have drifted automatically.
	 */
	void resizeToBottom ();

	bool isAtBottom() const;
	void commitCurrentViewportAsAnchor();

	Backend*						backend;
signals:
	void postEditInitiated (BackendPost& post);
	void scrolledToTop ();
	// Emitted only for wheel/keyboard/scrollbar input, never for layout-driven
	// scrollbar movement or automatic anchor restoration.
	void userViewportChanged (bool atBottom);
private:
	struct SavedScrollAnchor {
		QString postId;
		int bottomOffset = 0;
		bool atBottom = false;
		bool valid = false;
	};

	QList<QListWidgetItem*> sortedSelectedItems () const;
	void saveScrollAnchor();
	void scheduleSavedScrollAnchorRestore();
	void restoreSavedScrollAnchor(quint64 generation);
	void noteUserScrollIntent();
	void scheduleUserScrollAnchorUpdate();
	void commitUserScrollAnchor();
	void schedulePendingUserIntentReset();
	bool isUserScrollInProgress() const;

	void copySelectedItemsToClipboard (PostWidget::FormatType formatType);
	void keyPressEvent (QKeyEvent* event)		override;
	void wheelEvent (QWheelEvent* event) override;
	void resizeEvent (QResizeEvent* event) override;
	bool eventFilter(QObject* watched, QEvent* event) override;
	void focusOutEvent (QFocusEvent* event)		override;
	void showContextMenu (const QPoint &pos);
private:
	QTimer							removeNewMessagesSeparatorTimer;
	QListWidgetItem*				newMessagesSeparator;
	QListWidgetItem*				lastOwnPost;
	QListWidgetItem*				currentEditedItem;
	bool							menuShown;
	SavedScrollAnchor				savedScrollAnchor;
	bool							savedScrollRestoreScheduled = false;
	bool							userScrollAnchorUpdateScheduled = false;
	bool							restoringSavedScroll = false;
	bool							handlingUserScrollEvent = false;
	bool							scrollBarUserGesture = false;
	bool							pendingScrollBarUserIntent = false;
	quint64							scrollIntentGeneration = 0;
};

} /* namespace Mattermost */
