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

#include <QScrollBar>
#include <QDebug>
#include <set>
#include <QMenu>
#include <QApplication>
#include <QClipboard>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QResizeEvent>
#include <QTimer>
#include "post-separator/PostDaySeparatorWidget.h"
#include "backend/Backend.h"
#include "backend/types/BackendPost.h"
#include "info-dialogs/UserProfileDialog.h"
#include "PostsListWidget.h"
#include "choose-emoji-dialog/ChooseEmojiDialogWrapper.h"

namespace Mattermost {

PostsListWidget::PostsListWidget (QWidget* parent)
:ResizableListWidget (parent)
,backend (nullptr)
,newMessagesSeparator (nullptr)
,lastOwnPost (nullptr)
,currentEditedItem (nullptr)
,menuShown (false)
{
	removeNewMessagesSeparatorTimer.setSingleShot (true);
	connect (&removeNewMessagesSeparatorTimer, &QTimer::timeout, this, &PostsListWidget::removeNewMessagesSeparator);

	connect (this, &QListWidget::customContextMenuRequested, this, &PostsListWidget::showContextMenu);

	connect (verticalScrollBar(), &QAbstractSlider::valueChanged, [this] (int value) {
		if (value == 0 && verticalScrollBar()->maximum() != 0) {
			emit scrolledToTop ();
		}
	});
}

PostsListWidget::~PostsListWidget () = default;

void PostsListWidget::saveScrollAnchor()
{
	savedScrollAnchor = {};
	if (count() == 0 || viewport()->height() <= 0) {
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

		savedScrollAnchor.postId = postId;
		savedScrollAnchor.bottomOffset = viewportRect.bottom() - rect.bottom();
		savedScrollAnchor.valid = true;
		return;
	}
}

void PostsListWidget::clear()
{
	// Persist the last visible post rather than a scrollbar percentage. A ratio
	// is unstable when delayed images or Markdown reflow change row heights.
	saveScrollAnchor();

	removeNewMessagesSeparatorTimer.stop();
	QListWidget::clear();
	newMessagesSeparator = nullptr;
	lastOwnPost = nullptr;
	currentEditedItem = nullptr;
	savedScrollRestoreScheduled = false;
}

void PostsListWidget::scheduleSavedScrollAnchorRestore()
{
	if (!savedScrollAnchor.valid || savedScrollRestoreScheduled) {
		return;
	}

	savedScrollRestoreScheduled = true;

	// Initial row sizing is also queued with zero-timeout callbacks. Give those
	// callbacks one event-loop turn, then restore the saved post on the next one.
	// Subsequent image/content growth is handled by ResizableListWidget's live
	// viewport anchoring.
	QTimer::singleShot(0, this, [this] {
		QTimer::singleShot(0, this, [this] {
			savedScrollRestoreScheduled = false;
			restoreSavedScrollAnchor();
		});
	});
}

void PostsListWidget::restoreSavedScrollAnchor()
{
	if (!savedScrollAnchor.valid) {
		return;
	}

	const int row = findPostByIndex(savedScrollAnchor.postId, 0);
	if (row < 0) {
		return;
	}

	QListWidgetItem* listItem = item(row);
	if (!listItem) {
		return;
	}

	const int wantedBottomOffset = savedScrollAnchor.bottomOffset;
	const QPersistentModelIndex index = indexFromItem(listItem);
	const QString restoredPostId = savedScrollAnchor.postId;

	scrollToItem(listItem, QAbstractItemView::PositionAtBottom);
	QTimer::singleShot(0, this, [this, index, wantedBottomOffset, restoredPostId] {
		if (!index.isValid()) {
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
			verticalScrollBar()->setValue(verticalScrollBar()->value() + delta);
		}

		// The identity anchor has now been translated back into a live viewport
		// position. Future content growth keeps that viewport stable generically.
		savedScrollAnchor = {};
	});
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

	if (savedScrollAnchor.valid && savedScrollAnchor.postId == postWidget->post.id) {
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
}

void PostsListWidget::addDaySeparator (int insertPos, int daysAgo)
{
	PostSeparatorWidget* separator = new PostDaySeparatorWidget (daysAgo);
	QListWidgetItem* newItem = new QListWidgetItem();
	newItem->setData(Qt::UserRole, ItemType::separator);
	insertItem (insertPos, newItem);
	setItemWidget (newItem, separator);
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
}

void PostsListWidget::removeNewMessagesSeparator ()
{
	if (!newMessagesSeparator) {
		return;
	}

	delete newMessagesSeparator;
	newMessagesSeparator = nullptr;
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

	QListWidget::keyPressEvent (event);
}

/*
 * get selected items in the order, in which they appear in the PostsListWidget
 */
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
	if (verticalScrollBar()->maximum() - verticalScrollBar()->value() < 10) {
		scrollToBottom ();
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
