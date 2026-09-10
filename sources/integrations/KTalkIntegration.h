/**
 * @file KTalkIntegration.h
 * @brief Native adapter for a server-provided KTalk Mattermost plugin.
 *
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <QObject>
#include <QString>

#include "backend/HTTPConnector.h"
#include "backend/QByteArrayCreator.h"

class QAction;
class QJsonObject;
class QMainWindow;
class QToolBar;

namespace Mattermost {

class Backend;

/** Native UI adapter for a compatible server-advertised KTalk plugin. */
class KTalkIntegration final : public QObject
{
    Q_OBJECT
public:
    static KTalkIntegration& install(QMainWindow& window, Backend& backend);

private:
    KTalkIntegration(QMainWindow& window, Backend& backend);

    void refreshAvailability();
    void requestAppBarIcon();
    void startMeeting();
    void submitStartMeeting(const QString& channelId, bool callEveryone);
    void handleCustomWebSocketEvent(const QString& eventName,
                                    const QJsonObject& data);
    void showError(const QString& message);

    QMainWindow& window_;
    Backend& backend_;
    HTTPConnector httpConnector_;
    QToolBar* toolbar_ = nullptr;
    QAction* action_ = nullptr;
    QString pluginId_;
    bool iconRequested_ = false;
};

} // namespace Mattermost
