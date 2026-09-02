/**
 * @file PostsListWidget.cpp
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

#include <QScrollBar>
#include <QDebug>
#include <set>
#include <QMenu>
#include <QApplication>
#include <QClipboard>
#include <QKeyEvent>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QResizeEvent>
#include <QTimer>
#include <QWheelEvent>
#include "post-separator/PostDaySeparatorWidget.h"
#include "backend/Backend.h"
#include "backend/types/BackendPost.h"
#include "info-dialogs/UserProfileDialog.h"
#include "PostsListWidget.h"
#include "choose-emoji-dialog/ChooseEmojiDialogWrapper.h"

namespace Mattermost {

namespace {

bool isScrollNavigationKey(int key)
{
	switch (key) {
	case Qt::Key_Up:
	case Qt::Key_Down:
	case Qt::Key_PageUp:
	case Qt::Key_PageDown:
	case Qt::Key_Home:
	case Qt::Key_End:
		return true;
	default:
		return false;
	}
}

} // namespace

PostsListWidget::PostsListWidget (QWidget* parent)
:ResizableListWidget (parent)
,backend (nullptr)
,newMessagesSeparator (nullptr)
,lastOwnPost (nullptr)
,currentEditedItem (nullptr)
,menuShown (false)
{
	// Scroll anchors are expressed in pixels (post bottom relative to viewport
	// bottom), so keep the view's scrollbar in the same coordinate system on all
	// styles/platforms instead of relying on the style-dependent ScrollPerItem
	// default.
	setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);

	removeNewMessagesSeparatorTimer.setSingleShot (true);
	connect (&removeNewMessagesSeparatorTimer, &QTimer::timeout, this, &PostsListWidget::removeNewMessagesSeparator);

	connect (this, &QListWidget::customContextMenuRequested, this, &PostsListWidget::showContextMenu);

	QScrollBar* scrollBar = verticalScrollBar();
	scrollBar->installEventFilter(this);

	connect(scrollBar, &QAbstractSlider::sliderPressed, this, [this] {
		scrollBarUserGesture = true;
		noteUserScrollIntent();
	});
	connect(scrollBar, &QAbstractSlider::sliderReleased, this, [this] {
		scrollBarUserGesture = false;
		scheduleUserScrollAnchorUpdate();
	});
	connect(scrollBar, &QAbstractSlider::actionTriggered, this, [this](int) {
		noteUserScrollIntent();
		scheduleUserScrollAnchorUpdate();
	});
	connect(scrollBar, &QAbstractSlider::valueChanged, this, [this] (int value) {
		if (value == 0 && verticalScrollBar()->maximum() != 0) {
			emit scrolledToTop ();
		}

		if (restoringSavedScroll) {
			return;
		}

		if (isUserScrollInProgress()) {
			scheduleUserScrollAnchorUpdate();
		} else if (savedScrollAnchor.valid) {
			// A scrollbar value changed without corresponding user input. This is
			// a layout/programmatic movement and must never become the new reading
			// position. Put the user's anchor (or sticky bottom) back after layout.
			scheduleSavedScrollAnchorRestore();
		}
	});
}

PostsListWidget::~PostsListWidget () = default;

bool PostsListWidget::isAtBottom() const
{
	const QScrollBar* scrollBar = verticalScrollBar();
	return scrollBar->maximum() - scrollBar->value() <= 2;
}

bool PostsListWidget::isUserScrollInProgress() const
{
	return handlingUserScrollEvent || scrollBarUserGesture || pendingScrollBarUserIntent;
}

void PostsListWidget::saveScrollAnchor()
{
	SavedScrollAnchor anchor;
	anchor.atBottom = isAtBottom();

	if (count() == 0 || viewport()->height() <= 0) {
		savedScrollAnchor = anchor;
		return;
	}

	const QRect viewportRect = viewport()->rect();
	for (int row = count() - 1; row >= 0; --row) {
		QListWidgetItem* listItem = item(row);
		if (!isPostItem(listItem)) {
			continue;
		}

		const QRect rect = visualItemRect(listItem);
		if (!rect.isValid() || !rect.intersects(viewportRect)) {
			continue;
		}

		const QString postId = listItem->data(ItemRole::postId).toString();
		if (postId.isEmpty()) {
			continue;
		}

		anchor.postId = postId;
		anchor.bottomOffset = viewportRect.bottom() - rect.bottom();
		anchor.valid = true;
		break;
	}

	savedScrollAnchor = anchor;
}

void PostsListWidget::commitCurrentViewportAsAnchor()
{
	++scrollIntentGeneration;
	saveScrollAnchor();
}

void PostsListWidget::freezeCurrentViewportAnchor()
{
	++scrollIntentGeneration;
	if (!savedScrollAnchor.valid) {
		saveScrollAnchor();
	}
	if (savedScrollAnchor.valid) {
		// Hiding a chat must freeze a concrete reading position. Messages arriving
		// while the page is inactive must not keep advancing an old sticky bottom.
		savedScrollAnchor.atBottom = false;
	}
}

void PostsListWidget::noteUserScrollIntent()
{
	++scrollIntentGeneration;

	// User input immediately detaches both semantic navigation and sticky bottom.
	// If the gesture ends at the real bottom, commitUserScrollAnchor() enables
	// bottom following again from the final scrollbar position.
	if (!timelineNavigationPostId.isEmpty()) {
		clearTimelineNavigationLock();
	}
	if (savedScrollAnchor.valid) {
		savedScrollAnchor.atBottom = false;
	}

	pendingScrollBarUserIntent = true;
	schedulePendingUserIntentReset();
}

void PostsListWidget::schedulePendingUserIntentReset()
{
	QTimer::singleShot(0, this, [this] {
		pendingScrollBarUserIntent = false;
	});
}

void PostsListWidget::scheduleUserScrollAnchorUpdate()
{
	if (userScrollAnchorUpdateScheduled) {
		return;
	}
	userScrollAnchorUpdateScheduled = true;
	QTimer::singleShot(0, this, [this] {
		userScrollAnchorUpdateScheduled = false;
		commitUserScrollAnchor();
	});
}

void PostsListWidget::commitUserScrollAnchor()
{
	// A stale automatic restore may still be queued from before the input event.
	// Advancing the generation makes that callback harmless. saveScrollAnchor()
	// samples the final physical bottom state; that becomes the sticky marker.
	++scrollIntentGeneration;
	saveScrollAnchor();
	if (savedScrollAnchor.valid) {
		emit userViewportChanged(savedScrollAnchor.atBottom);
	}
}

void PostsListWidget::clear()
{
	// Do not sample the scrollbar here. clear() is itself an automatic action,
	// and the viewport may already have been displaced by an asynchronous layout
	// just before deactivation. Preserve the already committed user anchor.
	freezeCurrentViewportAnchor();

	removeNewMessagesSeparatorTimer.stop();
	QListWidget::clear();
	newMessagesSeparator = nullptr;
	lastOwnPost = nullptr;
	currentEditedItem = nullptr;
	savedScrollRestoreScheduled = false;
	userScrollAnchorUpdateScheduled = false;
}

void PostsListWidget::scheduleSavedScrollAnchorRestore()
{
	if (!timelineNavigationPostId.isEmpty()) {
		scheduleTimelineNavigationRestore();
		return;
	}

	if (!savedScrollAnchor.valid || savedScrollRestoreScheduled) {
		return;
	}

	savedScrollRestoreScheduled = true;
	const quint64 generation = scrollIntentGeneration;

	// QListView applies both row size hints and scrollbar range changes lazily.
	// Two event-loop turns place restoration after those two phases. Every later
	// image/layout change schedules another restore, so the same anchor remains
	// authoritative for the whole asynchronous reflow.
	QTimer::singleShot(0, this, [this, generation] {
		QTimer::singleShot(0, this, [this, generation] {
			savedScrollRestoreScheduled = false;
			restoreSavedScrollAnchor(generation);
		});
	});
}

void PostsListWidget::restoreSavedScrollAnchor(quint64 generation)
{
	if (!timelineNavigationPostId.isEmpty()) {
		scheduleTimelineNavigationRestore();
		return;
	}

	if (!savedScrollAnchor.valid || generation != scrollIntentGeneration) {
		return;
	}

	// atBottom is an explicit "follow latest" marker, not merely diagnostic
	// geometry. While it is set, every automatic layout/new-message restore keeps
	// the newest content visible. Semantic navigation and user scroll clear it.
	if (savedScrollAnchor.atBottom) {
		restoringSavedScroll = true;
		QListWidget::scrollToBottom();
		restoringSavedScroll = false;
		saveScrollAnchor();
		if (savedScrollAnchor.valid) {
			savedScrollAnchor.atBottom = true;
		}
		return;
	}

	int anchorRow = -1;
	for (int row = 0; row < count(); ++row) {
		QListWidgetItem* listItem = item(row);
		if (isPostItem(listItem)
			&& listItem->data(ItemRole::postId).toString() == savedScrollAnchor.postId) {
			anchorRow = row;
			break;
		}
	}
	if (anchorRow < 0) {
		return;
	}

	QListWidgetItem* listItem = item(anchorRow);
	if (!listItem) {
		return;
	}

	const int wantedBottomOffset = savedScrollAnchor.bottomOffset;
	const QPersistentModelIndex index = indexFromItem(listItem);
	const QString restoredPostId = savedScrollAnchor.postId;

	restoringSavedScroll = true;
	QListWidget::scrollToItem(listItem, QAbstractItemView::PositionAtBottom);
	restoringSavedScroll = false;

	QTimer::singleShot(0, this, [this, index, wantedBottomOffset, restoredPostId, generation] {
		if (generation != scrollIntentGeneration || !index.isValid()) {
			return;
		}

		QListWidgetItem* currentItem = itemFromIndex(index);
		if (!currentItem || currentItem->data(ItemRole::postId).toString() != restoredPostId) {
			return;
		}

		const QRect rect = visualItemRect(currentItem);
		if (!rect.isValid()) {
			return;
		}

		const int targetBottom = viewport()->rect().bottom() - wantedBottomOffset;
		const int delta = rect.bottom() - targetBottom;
		if (delta != 0) {
			restoringSavedScroll = true;
			verticalScrollBar()->setValue(verticalScrollBar()->value() + delta);
			restoringSavedScroll = false;
		}
	});
}

void PostsListWidget::scrollToItem(const QListWidgetItem* listItem, QAbstractItemView::ScrollHint hint)
{
	if (!listItem) {
		return;
	}

	// An explicit scrollToItem() is semantic navigation (go to pinned post,
	// unread separator, quote, etc.). It always detaches from sticky bottom.
	++scrollIntentGeneration;
	if (savedScrollAnchor.valid) {
		savedScrollAnchor.atBottom = false;
	}
	restoringSavedScroll = true;
	QListWidget::scrollToItem(listItem, hint);
	restoringSavedScroll = false;
	commitCurrentViewportAsAnchor();
	if (savedScrollAnchor.valid) {
		savedScrollAnchor.atBottom = false;
	}
	QTimer::singleShot(0, this, [this] {
		commitCurrentViewportAsAnchor();
		if (savedScrollAnchor.valid) {
			savedScrollAnchor.atBottom = false;
		}
	});
}

void PostsListWidget::scrollToBottom()
{
	if (!timelineNavigationPostId.isEmpty()) {
		scheduleTimelineNavigationRestore();
		return;
	}

	if (savedScrollAnchor.valid && !savedScrollAnchor.atBottom) {
		// Automatic callers (new-post/layout/population) are allowed to move to
		// the newest edge only while the explicit sticky-bottom marker is set.
		scheduleSavedScrollAnchorRestore();
		return;
	}

	// Initial population, or an already sticky viewport: remain attached to the
	// newest edge and establish that state as the durable anchor.
	restoringSavedScroll = true;
	QListWidget::scrollToBottom();
	restoringSavedScroll = false;
	commitCurrentViewportAsAnchor();
	if (savedScrollAnchor.valid) {
		savedScrollAnchor.atBottom = true;
	}
}

void PostsListWidget::insertPost (int position, PostWidget* postWidget)
{
	QListWidgetItem* newItem = new QListWidgetItem();
	newItem->setData(Qt::UserRole, ItemType::post);
	newItem->setData(ItemRole::postId, postWidget->post.id);
	newItem->setSizeHint (QSize (viewportSizeHint().width(), postWidget->heightForWidth(width())));
	insertItem (position, newItem);

	setItemWidget (newItem, postWidget);
	verticalScrollBar()->setSingleStep (10);

	if (postWidget->post.isOwnPost()) {
		lastOwnPost = newItem;
	}

	if (savedScrollAnchor.valid) {
		scheduleSavedScrollAnchorRestore();
	}

	// MessageContentWidget emits dimensionsChanged asynchronously after text/image
	// reflow. A post row may have been removed by the time that queued update is
	// delivered, so never retain a raw QListWidgetItem pointer in this callback.
	// QPersistentModelIndex is invalidated by row removal and QPointer protects
	// the widget side. Let ResizableListWidget coalesce the actual geometry work.
	const QPersistentModelIndex itemIndex = indexFromItem(newItem);
	QPointer<PostWidget> guardedPost(postWidget);
	connect (postWidget, &PostWidget::dimensionsChanged, this,
		[this, itemIndex, guardedPost] {
			if (!guardedPost || !itemIndex.isValid()) {
				return;
			}

			QListWidgetItem* currentItem = itemFromIndex(itemIndex);
			if (!currentItem || itemWidget(currentItem) != guardedPost.data()) {
				return;
			}

			scheduleItemResize(currentItem, guardedPost.data(), true);
			scheduleSavedScrollAnchorRestore();
		});
}

void PostsListWidget::insertPost (PostWidget* postWidget)
{
	return insertPost (count (), postWidget);
}

int PostsListWidget::findPostByIndex (const QString& postId, int startIndex)
{
	if (postId.isEmpty()) {
		return -1;
	}

	while (startIndex < count()) {
		QListWidgetItem* listItem = item (startIndex);
		if (!isPostItem (listItem)) {
			++startIndex;
			continue;
		}

		if (listItem->data(ItemRole::postId).toString() == postId) {
			return startIndex;
		}

		++startIndex;
	}

	qDebug() << "Post with id " << postId << " not found";
	return -1;
}

PostWidget* PostsListWidget::findPost (const QString& postId)
{
	if (postId.isEmpty()) {
		return nullptr;
	}

	const int row = findPostByIndex(postId, 0);
	if (row < 0) {
		return nullptr;
	}

	QListWidgetItem* listItem = item(row);
	return listItem ? static_cast<PostWidget*>(itemWidget(listItem)) : nullptr;
}

void PostsListWidget::scrollToUnreadPostsOrBottom ()
{
	if (newMessagesSeparator) {
		scrollToItem (newMessagesSeparator, QAbstractItemView::PositionAtCenter);
	} else {
		scrollToBottom ();
	}
}

void PostsListWidget::addDaySeparator (int daysAgo)
{
	PostSeparatorWidget* separator = new PostDaySeparatorWidget (daysAgo);
	QListWidgetItem* newItem = new QListWidgetItem();
	newItem->setData(Qt::UserRole, ItemType::separator);
	addItem (newItem);
	setItemWidget (newItem, separator);
	if (savedScrollAnchor.valid) {
		scheduleSavedScrollAnchorRestore();
	}
}

void PostsListWidget::addDaySeparator (int insertPos, int daysAgo)
{
	PostSeparatorWidget* separator = new PostDaySeparatorWidget (daysAgo);
	QListWidgetItem* newItem = new QListWidgetItem();
	newItem->setData(Qt::UserRole, ItemType::separator);
	insertItem (insertPos, newItem);
	setItemWidget (newItem, separator);
	if (savedScrollAnchor.valid) {
		scheduleSavedScrollAnchorRestore();
	}
}

void PostsListWidget::addNewMessagesSeparator ()
{
	if (newMessagesSeparator) {
		return;
	}

	PostSeparatorWidget* separator = new PostSeparatorWidget ("New messages");
	newMessagesSeparator = new QListWidgetItem();
	newMessagesSeparator->setData(Qt::UserRole, ItemType::separator);

	addItem (newMessagesSeparator);
	setItemWidget (newMessagesSeparator, separator);
	if (savedScrollAnchor.valid) {
		scheduleSavedScrollAnchorRestore();
	}
}

void PostsListWidget::removeNewMessagesSeparator ()
{
	if (!newMessagesSeparator) {
		return;
	}

	delete newMessagesSeparator;
	newMessagesSeparator = nullptr;
	if (savedScrollAnchor.valid) {
		scheduleSavedScrollAnchorRestore();
	}
}

void PostsListWidget::removeNewMessagesSeparatorAfterTimeout (int timeoutMs)
{
	if (newMessagesSeparator) {
		removeNewMessagesSeparatorTimer.start (timeoutMs);
	}
}

QListWidgetItem* PostsListWidget::getLastOwnPost () const
{
	return lastOwnPost;
}

void PostsListWidget::initiatePostEdit (QListWidgetItem& postItem)
{
	if (currentEditedItem) {
		qDebug () << "Post edit requested while editing post";
		return;
	}

	PostWidget* post = static_cast <PostWidget*> (itemWidget (&postItem));

	qDebug() << "Edit " << post->post.message;
	currentEditedItem = &postItem;
	postItem.setBackground(Qt::yellow);
	clearSelection ();
	emit postEditInitiated (post->post);
}

void PostsListWidget::postEditFinished ()
{
	if (currentEditedItem) {
		currentEditedItem->setBackground(QBrush());
		currentEditedItem = nullptr;
	}
}

void PostsListWidget::keyPressEvent (QKeyEvent* event)
{
	/*
	 * Handle the key sequence for 'Copy' and copy all posts to clipboard (properly formatted)
	 */
	if (event->matches (QKeySequence::Copy)) {
		copySelectedItemsToClipboard (PostWidget::entirePost);
		return;
	}

	const bool scrollKey = isScrollNavigationKey(event->key());
	if (scrollKey) {
		noteUserScrollIntent();
		handlingUserScrollEvent = true;
	}
	QListWidget::keyPressEvent (event);
	if (scrollKey) {
		handlingUserScrollEvent = false;
		scheduleUserScrollAnchorUpdate();
	}
}

void PostsListWidget::wheelEvent(QWheelEvent* event)
{
	noteUserScrollIntent();
	handlingUserScrollEvent = true;
	QListWidget::wheelEvent(event);
	handlingUserScrollEvent = false;
	scheduleUserScrollAnchorUpdate();
}

void PostsListWidget::resizeEvent(QResizeEvent* event)
{
	ResizableListWidget::resizeEvent(event);
	if (savedScrollAnchor.valid) {
		scheduleSavedScrollAnchorRestore();
	}
}

bool PostsListWidget::eventFilter(QObject* watched, QEvent* event)
{
	if (watched == verticalScrollBar()) {
		if (event->type() == QEvent::Wheel || event->type() == QEvent::MouseButtonPress) {
			noteUserScrollIntent();
		}
		if (event->type() == QEvent::MouseButtonRelease) {
			scheduleUserScrollAnchorUpdate();
		}
	}

	const bool result = ResizableListWidget::eventFilter(watched, event);

	if (watched != verticalScrollBar()
		&& savedScrollAnchor.valid
		&& (event->type() == QEvent::LayoutRequest || event->type() == QEvent::Resize)) {
		scheduleSavedScrollAnchorRestore();
	}

	return result;
}

/*
 * get selected items in the order, in which they appear in the PostsListWidget
 */
QList<QListWidgetItem*> PostsListListWidget_dummy_for_formatting;

QList<QListWidgetItem*> PostsListWidget::sortedSelectedItems () const
{
	auto cmp = [this] (const QListWidgetItem* lhs, const QListWidgetItem* rhs) {
		return row (lhs) < row (rhs);
	};

	std::set<QListWidgetItem*, decltype (cmp)> set (cmp);

	for (auto& item: selectedItems()) {
		set.insert (item);
	}

	QList<QListWidgetItem*> sortedItems;

	for (auto item: set) {
		sortedItems.push_back (item);
	}

	return sortedItems;
}

void PostsListWidget::copySelectedItemsToClipboard (PostWidget::FormatType formatType)
{
	QString str;
	for (auto& item: sortedSelectedItems ()) {

		if (isPostItem (item)) {
			PostWidget* post = static_cast <PostWidget*> (itemWidget (item));
			if (post)
				str += post->formatForClipboardSelection (formatType);
		}
	}

	qDebug() << "Copy Posts Selection";
	QApplication::clipboard()->setText (str);
}

void PostsListWidget::showContextMenu (const QPoint& pos)
{
	// Handle global position
	QPoint globalPos = mapToGlobal(pos);

	QListWidgetItem* pointedItem = itemAt(pos);

	if (!isPostItem (pointedItem)) {
		return;
	}

	uint32_t selectedItemsCount = selectedItems().size();

	PostWidget* post = static_cast <PostWidget*> (itemWidget (pointedItem));
	if (!post) {
		return;
	}

	if (post->post.isDeleted) {
		return;
	}

	// Create menu and insert some actions
	QMenu myMenu;

	if (post->post.isOwnPost()) {

		if (selectedItemsCount == 1) {
			myMenu.addAction ("Edit", [this, pointedItem, post] {
				initiatePostEdit (*pointedItem);
			});

			myMenu.addAction ("Delete", [this, post] {
				qDebug() << "Delete " << post->post.message;
				backend->deletePost (post->post.id);
			});

			myMenu.addSeparator();
		}
	}


	if (!post->hoveredLink.isEmpty() && selectedItemsCount == 1) {
		myMenu.addAction ("Copy link to clipboard", [this, post] {
			QApplication::clipboard()->setText (post->hoveredLink);
		});
	}

	QString selectedText = post->getSelectedText ();

	myMenu.addAction ("Reply in thread", [this, post] {
		if (!post->threadButton)
			post->addThreadButton();

		post->openThreadWindow();
	});

	if (!selectedText.isEmpty()) {
		myMenu.addAction ("Copy selected text", [this, post, selectedText] {
			qDebug() << "Copy selected text";
			QApplication::clipboard()->setText (selectedText);
		});
	}

	myMenu.addAction ("Copy entire post (formatted)", [this, post] {
		copySelectedItemsToClipboard (PostWidget::entirePost);
	});

	if (selectedItemsCount == 1) {
		myMenu.addAction ("Copy post message", [this, post] {
			copySelectedItemsToClipboard (PostWidget::messageOnly);
		});
	}

	myMenu.addAction ("Add emoji reaction", [this, post] {
		showEmojiDialog ([this, post] (Emoji emoji){
			backend->addPostReaction (post->post.id, emoji.name);
		});
	});

	myMenu.addSeparator();

	myMenu.addAction ("View " + post->post.author->getDisplayName() + "'s profile", [this, post] {
		UserProfileDialog* dialog = new UserProfileDialog (*post->post.author, this);
		dialog->show ();
	});

#if 0
	if (selectedItemsCount == 1) {
		myMenu.addAction ("Reply", [post] {
			qDebug() << "Reply " << post->post.message;
		});
	}

	myMenu.addAction ("Pin", [post] {
		qDebug() << "Pin " << post->post.message;
	});
#endif

	// Show context menu at handling position. And do not focus-out
	menuShown = true;
	myMenu.exec(globalPos + QPoint (10, 0));
	menuShown = false;
}

void PostsListWidget::resizeToBottom ()
{
	if (!timelineNavigationPostId.isEmpty()) {
		scheduleTimelineNavigationRestore();
		return;
	}

	if (savedScrollAnchor.valid) {
		scheduleSavedScrollAnchorRestore();
		return;
	}

	if (verticalScrollBar()->maximum() - verticalScrollBar()->value() < 10) {
		restoringSavedScroll = true;
		QListWidget::scrollToBottom ();
		restoringSavedScroll = false;
		commitCurrentViewportAsAnchor();
		if (savedScrollAnchor.valid) {
			savedScrollAnchor.atBottom = true;
		}
	}
}

void PostsListWidget::focusOutEvent (QFocusEvent* event)
{
	if (!menuShown) {
		clearSelection ();
	}
	QListWidget::focusOutEvent (event);
}

} /* namespace Mattermost */
