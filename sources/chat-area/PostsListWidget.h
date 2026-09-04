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

#include <QElapsedTimer>
#include <QPersistentModelIndex>
#include <QStringList>
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
	// Only sparse-timeline day separators carry this role. The ordinary
	// new-messages separator intentionally does not, so incremental reconciliation
	// can distinguish timeline decoration from semantic UI state.
	daySeparatorDays,
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

	// Sparse controller calls retained for source compatibility. Long-lived
	// paint freezes caused stale backing-store artefacts; the incremental timeline
	// transaction now performs its own strictly synchronous QWidget freeze.
	void setUpdatesEnabled(bool) {}

	using QListWidget::addItem;
	// During beginTimelineRebuild() this pointer overload reconciles a desired gap
	// row with the existing list instead of blindly appending another item.
	void addItem(QListWidgetItem* item);

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

	// Return concrete post identities that intersect the actual viewport. Sparse
	// pruning uses this instead of a stale single anchor so the visible logical
	// range can be protected explicitly before any row is evicted.
	QStringList visibleTimelinePostIds() const
	{
		QStringList result;
		const QRect viewportRect = viewport()->rect();
		for (int row = 0; row < count(); ++row) {
			const QListWidgetItem* listItem = item(row);
			if (!isPostItem(listItem)) {
				continue;
			}
			const QRect rect = visualItemRect(listItem);
			if (!rect.isValid() || !rect.intersects(viewportRect)) {
				continue;
			}
			const QString id = listItem->data(ItemRole::postId).toString();
			if (!id.isEmpty()) {
				result.push_back(id);
			}
		}
		return result;
	}

	// QListView applies sizeHint changes lazily. Sparse timeline code must commit
	// the new row/gap geometry before restoring a semantic viewport anchor;
	// otherwise the later Qt layout pass can move the scrollbar into a gap.
	void applyTimelineGeometryNow()
	{
		QListWidget::doItemsLayout();
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
	// Pruning uses a quiet period rather than merely checking sliderDown: repeated
	// wheel/key navigation is a sequence of short events, and rows must not be
	// evicted in the gaps between those events.
	bool hasRecentUserScroll(int quietPeriodMs) const;
	// Convert an "at bottom" anchor into the concrete last visible post. This is
	// used before hiding a chat so messages arriving while the page is inactive
	// cannot silently move the saved reading position to the newer bottom.
	void freezeCurrentViewportAnchor();

	// During a full sparse reconciliation the desired controller sequence is
	// described again, but an already materialized row must keep its PostWidget.
	// Let PostWidget construction cheaply detect that case before it repeats
	// Markdown/layout/profile/attachment work for a throwaway duplicate.
	bool canReuseTimelinePost(const QString& postId) const
	{
		if (!timelineReconcileActive || postId.isEmpty()) {
			return false;
		}
		for (int row = timelineReconcileCursor; row < count(); ++row) {
			const QListWidgetItem* candidate = item(row);
			if (isPostItem(candidate)
				&& candidate->data(ItemRole::postId).toString() == postId) {
				return true;
			}
		}
		return false;
	}

	/**
	 * Replace one materialized sparse-timeline post row with gap geometry in
	 * place. This is used by pruning so remote rows can be evicted without
	 * describing/reconciling the entire timeline or touching retained widgets.
	 */
	bool evictTimelinePostToGap(const QString& postId,
	                            int logicalIndex,
	                            int estimatedRowHeight);

	/**
	 * Sparse controllers still describe the whole desired timeline on every
	 * model change, but this is now a reconciliation transaction: existing post
	 * rows/widgets are retained, only newly materialized rows are inserted, and
	 * rows no longer present in PostTimeline are removed at commit time.
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
	// Sparse controllers use this to suspend automatic gap prefetch while an
	// explicit navigation target owns the viewport.
	bool hasTimelineNavigationLock() const { return !timelineNavigationPostId.isEmpty(); }

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
		// Sticky-bottom state: automatic new content follows the newest edge only
		// while this is true. User scroll and semantic navigation clear it.
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

	bool reconcileTimelinePost(PostWidget* postWidget);
	bool reconcileTimelineGap(QListWidgetItem* desiredGap);
	bool reconcileTimelineDaySeparator(int daysAgo);
	void removeTimelineRow(int row);
	void finishTimelineReconcile();
	void resumeTimelinePainting();

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
	QElapsedTimer					lastUserScrollActivity;
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
	bool							timelineReconcileActive = false;
	int							timelineReconcileCursor = 0;
	bool							timelinePaintingSuspended = false;
};

} /* namespace Mattermost */