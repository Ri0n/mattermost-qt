/**
 * @file ResizableListWidget.cpp
 * @brief QListWidget wrapper, which adjusts items' sizes when resized
 * @author Lyubomir Filipov
 * @date Apr 1, 2023
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

#include "ResizableListWidget.h"

#include <algorithm>

#include <QEvent>
#include <QLayout>
#include <QPointer>
#include <QResizeEvent>
#include <QScrollBar>
#include <QTimer>

namespace {
constexpr const char* RowResizePendingProperty = "_mattermostRowResizePending";
constexpr const char* ExternalViewportAnchorProperty = "_mattermostExternalViewportAnchor";
}

void ResizableListWidget::setItemWidget(QListWidgetItem* item, QWidget* widget)
{
    QListWidget::setItemWidget(item, widget);
    if (!item || !widget) {
        return;
    }

    widget->installEventFilter(this);

    const int initialHeight = std::max({
        1,
        widget->sizeHint().height(),
        widget->minimumSizeHint().height()
    });
    item->setSizeHint(QSize(std::max(1, viewport()->width()), initialHeight));

    // Initial row setup happens while a chat is being populated. Do not anchor
    // the viewport here: the caller may still need to scroll to unread posts or
    // restore a previously saved chat position after all rows are present.
    scheduleItemResize(item, widget, false);
}

bool ResizableListWidget::usesInternalViewportAnchoring() const
{
    if (property(ExternalViewportAnchorProperty).toBool()) {
        return false;
    }

    // PostsListWidget owns a semantic sparse-timeline anchor. Its anchor knows
    // whether the viewport represents a post, a gap, sticky bottom, an active
    // thumb seek or a navigation lock. This base class only knows physical rows
    // and therefore must never schedule a second delayed restore for that view.
    return !inherits("Mattermost::PostsListWidget");
}

ResizableListWidget::ViewportAnchor ResizableListWidget::captureViewportAnchor() const
{
    ViewportAnchor anchor;
    const QScrollBar* scrollBar = verticalScrollBar();
    anchor.atBottom = scrollBar->maximum() - scrollBar->value() <= 2;

    if (count() == 0 || viewport()->height() <= 0) {
        return anchor;
    }

    const QRect viewportRect = viewport()->rect();
    for (int row = count() - 1; row >= 0; --row) {
        QListWidgetItem* listItem = item(row);
        const QRect rect = visualItemRect(listItem);
        if (!rect.isValid() || !rect.intersects(viewportRect)) {
            continue;
        }

        anchor.index = indexFromItem(listItem);
        anchor.bottomOffset = viewportRect.bottom() - rect.bottom();
        break;
    }

    return anchor;
}

void ResizableListWidget::restoreViewportAnchor(const ViewportAnchor& anchor)
{
    // Even when the viewport happened to be at the bottom before a layout
    // change, preserve the concrete row that was visible there. Treating
    // "bottom" as a moving target makes newly inserted rows and delayed image
    // reflow silently advance the reading position.
    if (!anchor.index.isValid()) {
        if (anchor.atBottom) {
            scrollToBottom();
        }
        return;
    }

    QListWidgetItem* listItem = itemFromIndex(anchor.index);
    if (!listItem) {
        return;
    }

    const QRect rect = visualItemRect(listItem);
    if (!rect.isValid()) {
        return;
    }

    const int targetBottom = viewport()->rect().bottom() - anchor.bottomOffset;
    const int delta = rect.bottom() - targetBottom;
    if (delta != 0) {
        verticalScrollBar()->setValue(verticalScrollBar()->value() + delta);
    }
}

void ResizableListWidget::scheduleItemResize(QListWidgetItem* item, QWidget* widget, bool preserveViewport)
{
    if (!item || !widget || widget->property(RowResizePendingProperty).toBool()) {
        return;
    }

    const QPersistentModelIndex index = indexFromItem(item);
    QPointer<QWidget> guardedWidget(widget);
    widget->setProperty(RowResizePendingProperty, true);

    QTimer::singleShot(0, this, [this, index, guardedWidget, preserveViewport] {
        if (!guardedWidget) {
            return;
        }

        if (!index.isValid()) {
            guardedWidget->setProperty(RowResizePendingProperty, false);
            return;
        }

        QListWidgetItem* currentItem = itemFromIndex(index);
        if (!currentItem || itemWidget(currentItem) != guardedWidget.data()) {
            guardedWidget->setProperty(RowResizePendingProperty, false);
            return;
        }

        const bool preserveHere = preserveViewport && usesInternalViewportAnchoring();
        const ViewportAnchor anchor = preserveHere ? captureViewportAnchor() : ViewportAnchor{};

        const int rowWidth = std::max(1, viewport()->width());
        if (guardedWidget->width() != rowWidth) {
            guardedWidget->resize(rowWidth, std::max(1, guardedWidget->height()));
        }

        if (QLayout* itemLayout = guardedWidget->layout()) {
            itemLayout->activate();
        }
        guardedWidget->updateGeometry();

        const int heightForWidth = guardedWidget->heightForWidth(rowWidth);
        const int targetHeight = std::max({
            1,
            heightForWidth,
            guardedWidget->sizeHint().height(),
            guardedWidget->minimumSizeHint().height()
        });

        currentItem->setSizeHint(QSize(rowWidth, targetHeight));
        guardedWidget->setProperty(RowResizePendingProperty, false);

        if (preserveHere) {
            // QListView may defer applying the new size hint until the current
            // event has returned. Restore the same bottom-most visible row once
            // that layout pass has completed. Sparse PostsListWidget deliberately
            // skips this callback and lets its semantic anchor own the viewport.
            QTimer::singleShot(0, this, [this, anchor] {
                restoreViewportAnchor(anchor);
            });
        }
    });
}

void ResizableListWidget::scheduleAllItemResizes(bool preserveViewport)
{
    for (int i = 0; i < count(); ++i) {
        QListWidgetItem* listItem = item(i);
        QWidget* widget = itemWidget(listItem);
        if (widget) {
            scheduleItemResize(listItem, widget, preserveViewport);
        }
    }
}

bool ResizableListWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::LayoutRequest || event->type() == QEvent::Resize) {
        auto* widget = qobject_cast<QWidget*>(watched);
        if (widget) {
            for (int i = 0; i < count(); ++i) {
                QListWidgetItem* listItem = item(i);
                if (itemWidget(listItem) == widget) {
                    scheduleItemResize(listItem, widget, true);
                    break;
                }
            }
        }
    }

    return QListWidget::eventFilter(watched, event);
}

void ResizableListWidget::resizeEvent(QResizeEvent* event)
{
    const bool preserveHere = usesInternalViewportAnchoring();

    // Capture before QAbstractItemView recomputes row geometry. At this point
    // the viewport still represents the old layout, so the bottom-most visible
    // row is the right visual anchor for an ordinary ResizableListWidget. Sparse
    // PostsListWidget owns this transaction itself and deliberately skips it.
    const ViewportAnchor anchor = preserveHere ? captureViewportAnchor() : ViewportAnchor{};

    QListWidget::resizeEvent(event);
    scheduleAllItemResizes(false);

    if (preserveHere) {
        QTimer::singleShot(0, this, [this, anchor] {
            restoreViewportAnchor(anchor);
        });
    }
}
