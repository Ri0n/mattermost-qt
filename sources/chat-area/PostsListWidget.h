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

#include <QPersistentModelIndex>
#include <QTimer>
#include <QVariantAnimation>

#include "ResizableListWidget.h"
#include "post/PostWidget.h"

namespace Mattermost {

class PostWidget;

namespace ItemType {
enum id {
	post,
	separator,
	// Sparse timeline placeholder. It intentionally has no item widget and no
	// painted background; only its sizeHint contributes to scroll geometry.
	gap,
};
}

namespace ItemRole {
enum id {
	postId = Qt::UserRole + 1,
	gapFirstIndex,
	gapCount,
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

	void highlightPost(const QString& postId)
	{
		const int row = findPostByIndex(postId, 0);
		if (row < 0) {
			return;
		}

		QListWidgetItem* listItem = item(row);
		if (!listItem) {
			return;
		}

		const QPersistentModelIndex index = indexFromItem(listItem);
		auto* animation = new QVariantAnimation(this);
		animation->setDuration(1400);

		QColor start = palette().color(QPalette::Highlight);
		start.setAlpha(120);
		QColor finish = start;
		finish.setAlpha(0);
		animation->setStartValue(start);
		animation->setEndValue(finish);
		animation->setEasingCurve(QEasingCurve::OutCubic);

		connect(animation, &QVariantAnimation::valueChanged, this,
		        [this, index](const QVariant& value) {
			if (!index.isValid()) {
				return;
			}
			if (QListWidgetItem* current = itemFromIndex(index)) {
				current->setBackground(QBrush(value.value<QColor>()));
			}
		});
		connect(animation, &QVariantAnimation::finished, this, [this, index] {
			if (index.isValid()) {
				if (QListWidgetItem* current = itemFromIndex(index)) {
					current->setBackground(QBrush());
				}
			}
		});
		animation->start(QAbstractAnimation::DeleteWhenStopped);
	}

	// Sparse rebuilds run synchronously on the GUI thread. Freezing QWidget
	// updates across later event-loop turns leaves stale backing-store pixels when
	// the splitter/list geometry changes (observed as post text over the editor).
	// Qt already coalesces paints until control returns to the event loop, so the
	// timeline controllers' legacy paint-suspension calls are intentionally no-op.
	void setUpdatesEnabled(bool) {}

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

	static bool isGapItem(const QListWidgetItem* item)
	{
		if (!item) {
			return false;
		}
		const QVariant itemType = item->data(Qt::UserRole);
		return itemType.isValid() && itemType.toInt() == ItemType::gap;
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
	// Convert an "at bottom" anchor into the concrete last visible post. This is
	// used before hiding a chat so messages arriving while it is inactive cannot
	// silently move the saved reading position to the newer bottom.
	void freezeCurrentViewportAnchor();

	/**
	 * Sparse timeline controllers replace a large part of the QListWidget in one
	 * operation. Suppress the ordinary per-row anchor restoration while that
	 * transaction is in progress, then establish exactly one final viewport.
	 */
	void beginTimelineRebuild();
	void finishTimelineRebuildAtBottom();
	bool finishTimelineRebuildAtPost(const QString& postId, int viewportTopOffset);
	void finishTimelineRebuildAtPixel(qint64 pixelOffset);

	/**
	 * Keep a semantic navigation target fixed while sparse pages and attachment
	 * geometry settle. A quietPeriodMs of zero keeps the lock until explicit user
	 * scrolling or list teardown; positive values retain the old timed behavior.
	 */
	void lockTimelineNavigationToPost(const QString& postId,
	                                  int viewportTopOffset = 0,
	                                  int quietPeriodMs = 2000);
	void clearTimelineNavigationLock();

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
	bool restoreTimelineNavigationLock();
	void touchTimelineNavigationLock();
	void scheduleTimelineNavigationRestore();

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
	QString							timelineNavigationPostId;
	int							timelineNavigationTopOffset = 0;
	int							timelineNavigationQuietPeriodMs = 2000;
	quint64							timelineNavigationLockGeneration = 0;
	bool							timelineNavigationUserCancelConnected = false;
	bool							timelineNavigationGeometryConnected = false;
	bool							timelineNavigationRestoreScheduled = false;
};

} /* namespace Mattermost */