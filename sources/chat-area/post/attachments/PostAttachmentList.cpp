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

#include "PostAttachmentList.h"
#include "ui_PostAttachmentList.h"

#include <QDebug>
#include <QLabel>
#include <QListWidgetItem>
#include <QVBoxLayout>
#include "AttachedBinaryFile.h"
#include "AttachedImageFile.h"
#include "AttachedVideoFile.h"
#include "backend/types/BackendFile.h"

namespace Mattermost {

PostAttachmentList::PostAttachmentList (Backend& backend, QWidget *parent)
:QWidget(parent)
,backend (backend)
,ui (new Ui::PostAttachmentList)
{
    ui->setupUi(this);
    ui->verticalLayout->setContentsMargins(0, 0, 0, 0);
    ui->listWidget->viewport()->setAutoFillBackground(false);
    ui->listWidget->setSpacing(10);
    ui->listWidget->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    ui->listWidget->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    ui->listWidget->setStyleSheet(QStringLiteral(
        "QListWidget::item { border: 1px solid black; }"));
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

PostAttachmentList::~PostAttachmentList()
{
    delete ui;
}

void PostAttachmentList::addFile (const BackendFile& file, const QString& authorName)
{
    auto* newItem = new QListWidgetItem();
    QWidget* fileWidget = nullptr;
    AttachedImageFile* imageWidget = nullptr;

#if BUILD_MULTIMEDIA
    if (file.name.endsWith(".mp4", Qt::CaseInsensitive) || file.name.endsWith(".mov", Qt::CaseInsensitive)) {
        fileWidget = new AttachedVideoFile (backend, file, this);
    } else
#endif
    if (!file.mimeType.startsWith("image")) {
        fileWidget = new AttachedBinaryFile (backend, file, this);
    } else {
        imageWidget = new AttachedImageFile (backend, file, authorName, this);
        fileWidget = imageWidget;
    }

    // QListWidget paints the item border underneath the widget installed with
    // setItemWidget(). If the attachment widget occupies the complete item
    // rectangle, an opaque image covers the left/right/bottom border pixels.
    // Keep the content one physical layout pixel inside the item on every side
    // so the delegate-owned border and the attachment geometry cannot overlap.
    fileWidget->adjustSize();

    auto* itemWidget = new QWidget();
    auto* itemLayout = new QVBoxLayout(itemWidget);
    itemLayout->setContentsMargins(1, 1, 1, 1);
    itemLayout->setSpacing(0);
    itemLayout->addWidget(fileWidget);
    itemWidget->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    ui->listWidget->addItem(newItem);
    ui->listWidget->setItemWidget(newItem, itemWidget);

    const auto syncItemGeometry = [this, newItem, fileWidget, itemWidget, itemLayout] {
        const QSize contentSize = fileWidget->size().expandedTo(QSize(1, 1));
        const QMargins margins = itemLayout->contentsMargins();
        const QSize itemSize(
            contentSize.width() + margins.left() + margins.right(),
            contentSize.height() + margins.top() + margins.bottom());

        itemWidget->setFixedSize(itemSize);
        newItem->setSizeHint(itemSize);
        updateDimensions();
    };

    if (imageWidget) {
        connect(imageWidget, &AttachedImageFile::dimensionsChanged,
                this, syncItemGeometry);
    }

    syncItemGeometry();
}

void PostAttachmentList::updateDimensions()
{
    const QSize listSize = ui->listWidget->sizeHint().expandedTo(QSize(1, 1));
    ui->listWidget->setFixedSize(listSize);

    if (layout()) {
        layout()->activate();
        setFixedSize(layout()->sizeHint());
    } else {
        setFixedSize(listSize);
    }

    updateGeometry();
    emit dimensionsChanged();
}

} /* namespace Mattermost */
