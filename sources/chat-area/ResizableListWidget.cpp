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
#include <QPersistentModelIndex>
#include <QPointer>
#include <QResizeEvent>
#include <QTimer>

namespace {
constexpr const char* RowResizePendingProperty = "_mattermostRowResizePending";
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
    scheduleItemResize(item, widget);
}

void ResizableListWidget::scheduleItemResize(QListWidgetItem* item, QWidget* widget)
{
    if (!item || !widget || widget->property(RowResizePendingProperty).toBool()) {
        return;
    }

    const QPersistentModelIndex index = indexFromItem(item);
    QPointer<QWidget> guardedWidget(widget);
    widget->setProperty(RowResizePendingProperty, true);

    QTimer::singleShot(0, this, [this, index, guardedWidget] {
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
    });
}

void ResizableListWidget::scheduleAllItemResizes()
{
    for (int i = 0; i < count(); ++i) {
        QListWidgetItem* listItem = item(i);
        QWidget* widget = itemWidget(listItem);
        if (widget) {
            scheduleItemResize(listItem, widget);
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
                    scheduleItemResize(listItem, widget);
                    break;
                }
            }
        }
    }

    return QListWidget::eventFilter(watched, event);
}

void ResizableListWidget::resizeEvent(QResizeEvent* event)
{
    QListWidget::resizeEvent(event);
    scheduleAllItemResizes();
}
