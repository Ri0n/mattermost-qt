/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include "OutgoingPostCreator.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QMimeData>
#include <QPixmap>

namespace Mattermost {

void OutgoingPostCreator::insertFromMimeData(const QMimeData* source)
{
    if (!source || !source->hasImage()) {
        MessageTextEditWidget::insertFromMimeData(source);
        return;
    }

    // Keep the attachment set immutable once sendPostButtonAction() has
    // snapshotted it into OutgoingPostData. This mirrors attach-button and
    // drag/drop behaviour while an upload/send is in progress.
    if (isWaitingForPostServerResponse()) {
        qDebug() << "Cannot paste an image while sending a post";
        return;
    }

    const QVariant imageData = source->imageData();
    QImage image;
    if (imageData.canConvert<QImage>()) {
        image = qvariant_cast<QImage>(imageData);
    }
    if (image.isNull() && imageData.canConvert<QPixmap>()) {
        image = qvariant_cast<QPixmap>(imageData).toImage();
    }

    if (image.isNull() || !attachmentParent) {
        // Preserve QTextEdit's normal paste semantics when an advertised image
        // cannot actually be decoded, or when the composer is not initialized.
        MessageTextEditWidget::insertFromMimeData(source);
        return;
    }

    if (!clipboardAttachmentDir) {
        auto directory = std::make_unique<QTemporaryDir>(
            QDir::tempPath() + QStringLiteral("/mattermost-qt-clipboard-XXXXXX"));
        if (!directory->isValid()) {
            qWarning() << "Cannot create temporary directory for pasted image";
            MessageTextEditWidget::insertFromMimeData(source);
            return;
        }
        clipboardAttachmentDir = std::move(directory);
    }

    const QString baseName = QStringLiteral("clipboard-image-%1")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")));
    QString fileName = baseName + QStringLiteral(".png");
    QString filePath = clipboardAttachmentDir->filePath(fileName);
    int suffix = 2;
    while (QFileInfo::exists(filePath)) {
        fileName = baseName + QStringLiteral("-%1.png").arg(suffix++);
        filePath = clipboardAttachmentDir->filePath(fileName);
    }

    if (!image.save(filePath, "PNG")) {
        qWarning() << "Cannot encode pasted clipboard image as PNG";
        MessageTextEditWidget::insertFromMimeData(source);
        return;
    }

    QStringList files {filePath};
    createAttachmentList(files);
}

} // namespace Mattermost
