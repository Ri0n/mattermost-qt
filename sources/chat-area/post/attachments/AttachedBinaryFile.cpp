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

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QMimeDatabase>
#include <QPointer>
#include <QStyle>

#include "Settings.h"
#include "AttachedBinaryFile.h"
#include "ui_AttachedBinaryFile.h"
#include "backend/Backend.h"
#include "backend/types/BackendFile.h"
#include "config/Config.h"

namespace Mattermost {

AttachedBinaryFile::AttachedBinaryFile(Backend& backend, const BackendFile& file, QWidget* parent)
    : QWidget(parent)
    , ui(new Ui::AttachedBinaryFile)
{
    ui->setupUi(this);
    ui->fileNameLabel->setText("File: " + file.name);
    ui->downloadedLabel->setText("");

    static QLocale locale = QLocale::system();
    ui->fileSizeLabel->setText(
        "Size: " + locale.formattedDataSize(file.size, 2, QLocale::DataSizeTraditionalFormat));

    setFileMimeIcon(file.name);

    const QString fileId = file.id;
    const QString fileName = file.name;
    const uint64_t fileSize = file.size;

    connect(ui->downloadButton, &QPushButton::clicked, this,
            [this, &backend, fileId, fileName, fileSize] {
        QSettings settings;
        const QDir downloadDir = settings.value(DOWNLOAD_LOCATION, QDir::currentPath()).toString();
        const QString fileDestination = downloadDir.filePath(fileName);
        const QFileInfo fileInfo(fileDestination);

        if (fileInfo.isFile() && static_cast<uint64_t>(fileInfo.size()) == fileSize) {
            QMessageBox msgBox(
                QMessageBox::Question,
                "File exists - Mattermost",
                "The file '" + fileName + "' is already downloaded to \n'" + downloadDir.absolutePath() + "'",
                QMessageBox::NoButton,
                this);
            msgBox.setInformativeText("Please choose:");
            QPushButton* downloadAgainButton = msgBox.addButton("Download Again", QMessageBox::AcceptRole);
            QPushButton* openButton = msgBox.addButton("Open File", QMessageBox::AcceptRole);
            msgBox.setStandardButtons(QMessageBox::Cancel);
            msgBox.setDefaultButton(QMessageBox::Cancel);
            msgBox.exec();

            if (msgBox.clickedButton() == msgBox.button(QMessageBox::Cancel)) {
                return;
            }
            if (msgBox.clickedButton() == openButton) {
                QDesktopServices::openUrl(QUrl::fromLocalFile(fileDestination));
                return;
            }
            if (msgBox.clickedButton() != downloadAgainButton) {
                return;
            }
        }

        ui->openButton->setDisabled(true);
        ui->downloadedLabel->setText("Downloading...");

        QPointer<AttachedBinaryFile> self(this);
        backend.retrieveFile(fileId, [self, fileName, downloadDir](const QByteArray& fileData) {
            if (!self) {
                return;
            }

            const QString fileDestination = downloadDir.filePath(fileName);
            QFile destFile(fileDestination);
            if (!destFile.open(QIODevice::WriteOnly)) {
                self->ui->downloadedLabel->setText("Failed to save file: " + destFile.errorString());
                self->ui->openButton->setDisabled(false);
                return;
            }

            destFile.write(fileData);
            destFile.close();
            self->ui->downloadedLabel->setText(
                "File downloaded to '" + downloadDir.absolutePath() + "'");
            self->downloadedPath = fileDestination;
            self->ui->openButton->setDisabled(false);
        });
    });

    connect(ui->openButton, &QPushButton::clicked, this,
            [this, &backend, fileId, fileName] {
        if (!downloadedPath.isEmpty()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(downloadedPath));
            return;
        }

        QPointer<AttachedBinaryFile> self(this);
        backend.retrieveFile(fileId, [self, fileName](const QByteArray& fileData) {
            if (!self) {
                return;
            }

            QString tmpName(fileName);
            int dot = tmpName.lastIndexOf(QLatin1Char('.'));
            if (dot < 0) {
                dot = tmpName.size();
            }
            tmpName.insert(dot, QStringLiteral("XXXXXX"));

            self->tempFile.setFileTemplate(Config::tempDirectory().filePath(tmpName));
            if (!self->tempFile.open()) {
                qDebug() << self->tempFile.errorString();
                return;
            }

            self->tempFile.write(fileData);
            self->tempFile.close();
            QDesktopServices::openUrl(QUrl::fromLocalFile(self->tempFile.fileName()));
        });
    });
}

AttachedBinaryFile::~AttachedBinaryFile()
{
    delete ui;
}

void AttachedBinaryFile::setFileMimeIcon(const QString& filename)
{
    static QMimeDatabase mimeDatabase;

    const QMimeType mimeType = mimeDatabase.mimeTypeForUrl(filename);
    QIcon icon = QIcon::fromTheme(mimeType.iconName());

    ui->fileTypeLabel->setText("Type: " + mimeType.name());

    if (icon.isNull()) {
        icon = QApplication::style()->standardIcon(QStyle::SP_FileIcon);
    }

    if (!icon.isNull()) {
        const QPixmap pixmap = icon.pixmap(QSize(64, 64));
        ui->fileIcon->setPixmap(pixmap);
        ui->fileIcon->setFixedSize(pixmap.size());
    }
}

} /* namespace Mattermost */
