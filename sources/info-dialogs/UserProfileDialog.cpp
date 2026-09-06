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

#include "UserProfileDialog.h"
#include "ui_UserProfileDialog.h"

#include <memory>

#include <QDialogButtonBox>
#include <QLayoutItem>
#include <QNetworkRequest>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>

#include "backend/Backend.h"
#include "backend/NetworkRequest.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendDirectChannelsTeam.h"
#include "backend/types/BackendUser.h"
#include "navigation/AppNavigationService.h"

namespace Mattermost {

static QString getString(const QString& str)
{
    return str.isEmpty() ? QStringLiteral("N/A") : str;
}

UserProfileDialog::UserProfileDialog(const BackendUser& user, QWidget* parent)
    : UserProfileDialog(nullptr, user, parent)
{
}

UserProfileDialog::UserProfileDialog(Backend& backend, const BackendUser& user, QWidget* parent)
    : UserProfileDialog(&backend, user, parent)
{
}

UserProfileDialog::UserProfileDialog(Backend* backendInstance,
                                     const BackendUser& user,
                                     QWidget* parent)
    : QDialog(parent)
    , ui(new Ui::UserProfileDialog)
    , backend(backendInstance)
    , userId(user.id)
{
    ui->setupUi(this);

    // Keep the primary profile action on the same baseline as Close. ActionRole
    // is ordered on the opposite side of the standard reject/close buttons by
    // the active platform style, instead of maintaining a second button row.
    ui->buttonBox->addButton(ui->messageButton, QDialogButtonBox::ActionRole);
    while (QLayoutItem* item = ui->actionsLayout->takeAt(0)) {
        delete item;
    }

    setWindowTitle(QStringLiteral("Profile for ") + user.getDisplayName()
                   + QStringLiteral(" - Mattermost"));

    constexpr int ProfileAvatarSize = 128;
    if (!user.avatar.isNull()) {
        ui->avatar->setPixmap(user.avatar.scaled(ProfileAvatarSize,
                                                  ProfileAvatarSize,
                                                  Qt::KeepAspectRatio,
                                                  Qt::SmoothTransformation));
    } else {
        ui->avatar->clear();
    }
    ui->avatar->setAlignment(Qt::AlignCenter);

    if (!user.id.isEmpty()) {
        const QString pictureVersion = QString::number(
            static_cast<qulonglong>(user.last_picture_update));
        NetworkRequest request(
            QStringLiteral("users/") + user.id + QStringLiteral("/image?_=")
                + pictureVersion,
            true);
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                             QNetworkRequest::PreferCache);

        QPointer<UserProfileDialog> guard(this);
        avatarConnector.get(request, HttpResponseCallback(
            [guard](QByteArray avatarData) {
                if (!guard) {
                    return;
                }
                QPixmap pixmap;
                if (!pixmap.loadFromData(avatarData)) {
                    return;
                }
                guard->ui->avatar->setPixmap(
                    pixmap.scaled(ProfileAvatarSize,
                                  ProfileAvatarSize,
                                  Qt::KeepAspectRatio,
                                  Qt::SmoothTransformation));
            }));
    }

    ui->fullnameValue->setText(user.first_name + QLatin1Char(' ') + user.last_name);
    ui->nicknameValue->setText(getString(user.nickname));
    ui->usernameValue->setText(user.username);
    ui->emailValue->setText(user.email);
    ui->positionValue->setText(getString(user.position));
    ui->statusValue->setText(user.status);
    ui->timezoneValue->setText(user.timezone.automaticTimezone);

    ui->messageButton->setVisible(backend != nullptr && !user.id.isEmpty());
    if (backend) {
        connect(ui->messageButton, &QPushButton::clicked,
                this, &UserProfileDialog::startDirectMessage);
    }
}

UserProfileDialog::~UserProfileDialog()
{
    delete ui;
}

void UserProfileDialog::startDirectMessage()
{
    if (!backend || userId.isEmpty()) {
        return;
    }

    BackendUser* user = backend->getStorage().getUserById(userId);
    if (!user) {
        return;
    }

    if (BackendChannel* channel = backend->getStorage().getDirectChannelByUserId(userId)) {
        AppNavigationService::instance(*backend).openChannel(channel->id);
        accept();
        return;
    }

    // New direct channels arrive asynchronously through the ordinary backend
    // channel path. Keep one backend-owned connection alive after this dialog
    // closes and navigate exactly once when the requested user's DM appears.
    Backend* backendInstance = backend;
    const QString requestedUserId = userId;
    auto connection = std::make_shared<QMetaObject::Connection>();
    *connection = connect(&backendInstance->getStorage().directChannels,
                          &BackendDirectChannelsTeam::onNewChannel,
                          backendInstance,
                          [backendInstance, requestedUserId, connection](BackendChannel& channel) {
        if (channel.type != BackendChannel::directChannel
            || channel.name != requestedUserId) {
            return;
        }
        QObject::disconnect(*connection);
        AppNavigationService::instance(*backendInstance).openChannel(channel.id);
    });

    ui->messageButton->setEnabled(false);
    ui->messageButton->setText(tr("Starting…"));
    backend->createDirectChannel(*user);
    accept();
}

} /* namespace Mattermost */
