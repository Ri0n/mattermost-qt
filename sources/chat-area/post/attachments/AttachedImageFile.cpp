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

#include "AttachedImageFile.h"
#include "ui_AttachedImageFile.h"

#include <QDebug>
#include <QFileDialog>
#include <QMenu>
#include <QPointer>
#include <QSettings>

#include "backend/Backend.h"
#include "backend/types/BackendFile.h"
#include "Settings.h"

namespace Mattermost {

std::map<const QWidget*, FilePreview*> AttachedImageFile::currentlyOpenFiles;

AttachedImageFile::AttachedImageFile(Backend& backend, const BackendFile& file, const QString&, QWidget* parent)
    : QWidget(parent)
    , ui(new Ui::AttachedImageFile)
    , file(file)
    , backend(backend)
{
    ui->setupUi(this);
    ui->imageName->setText(file.name);

    const QString fileId = file.id;
    const QString fileName = file.name;
    QPointer<AttachedImageFile> self(this);

    backend.retrieveFile(fileId, [self](const QByteArray& fileContents) {
        if (!self) {
            return;
        }

        QSettings settings;
        const int maxWidth = settings.value(DOWNLOAD_IMAGE_MAX_WIDTH, 500).toInt();
        const int maxHeight = settings.value(DOWNLOAD_IMAGE_MAX_HEIGHT, 500).toInt();

        QPixmap pixmap;
        pixmap.loadFromData(fileContents);
        if (pixmap.width() > maxWidth) {
            pixmap = pixmap.scaledToWidth(maxWidth, Qt::SmoothTransformation);
        }
        if (pixmap.height() > maxHeight) {
            pixmap = pixmap.scaledToHeight(maxHeight, Qt::SmoothTransformation);
        }

        self->ui->imagePreview->setPixmap(pixmap);
        self->ui->imagePreview->adjustSize();
        self->adjustSize();
        emit self->dimensionsChanged();
    });

    connect(this, &QWidget::customContextMenuRequested, this,
            [this, fileId, fileName](const QPoint& pos) {
        QMenu menu(this);

        menu.addAction("Save image", this, [this, fileId, fileName] {
            QSettings settings;
            const QDir downloadDir = settings.value(DOWNLOAD_LOCATION, QDir::currentPath()).toString();
            const QString saveFileDestination = QFileDialog::getSaveFileName(
                this, "Save image as... - Mattermost", downloadDir.filePath(fileName));

            if (saveFileDestination.isEmpty()) {
                return;
            }

            backend.retrieveFile(fileId, [saveFileDestination](const QByteArray& fileContents) {
                QFile destFile(saveFileDestination);
                if (!destFile.open(QIODevice::WriteOnly)) {
                    qWarning() << "Cannot save image to" << saveFileDestination << ":" << destFile.errorString();
                    return;
                }
                destFile.write(fileContents);
            });
        });

        menu.exec(mapToGlobal(pos) + QPoint(10, 0));
    });
}

AttachedImageFile::~AttachedImageFile()
{
    currentlyOpenFiles.erase(this);
    delete ui;
}

void AttachedImageFile::mouseReleaseEvent(QMouseEvent*)
{
    qDebug() << "mouseRelease";

    const QString fileId = file.id;
    QPointer<AttachedImageFile> self(this);
    backend.retrieveFile(fileId, [self](const QByteArray& fileContents) {
        if (!self) {
            return;
        }

        const QWidget* const key = self.data();
        auto openFile = currentlyOpenFiles.find(key);
        FilePreview* filePreview = nullptr;

        if (openFile == currentlyOpenFiles.end()) {
            FilePreviewData previewData { fileContents, "", "" };
            filePreview = new FilePreview(previewData, nullptr);
            currentlyOpenFiles.emplace(key, filePreview);
            filePreview->setAttribute(Qt::WA_DeleteOnClose);
            filePreview->show();

            connect(filePreview, &QDialog::rejected, filePreview, [key, filePreview] {
                qDebug() << "Rejected";
                auto it = AttachedImageFile::currentlyOpenFiles.find(key);
                if (it != AttachedImageFile::currentlyOpenFiles.end() && it->second == filePreview) {
                    AttachedImageFile::currentlyOpenFiles.erase(it);
                }
            });
        } else {
            filePreview = openFile->second;
            filePreview->raise();
            filePreview->activateWindow();
        }
    });
}

} /* namespace Mattermost */
