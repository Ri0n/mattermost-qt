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

#include <QIcon>
#include <QObject>
#include <QString>

#include "backend/HTTPConnector.h"
#include "backend/QByteArrayCreator.h"

class QJsonObject;
class QWidget;

namespace Mattermost {

class Backend;

/** Native adapter for a compatible server-advertised KTalk plugin. */
class KTalkIntegration final : public QObject
{
    Q_OBJECT
public:
    static KTalkIntegration& instance(Backend& backend);

    bool isAvailable() const { return !pluginId_.isEmpty(); }
    const QIcon& icon() const { return icon_; }

    /**
     * Start a meeting in the supplied conversation context. A non-empty rootId
     * asks the server plugin to attach the meeting post to that thread.
     */
    void startMeeting(QWidget* parent,
                      const QString& channelId,
                      const QString& rootId = QString());

signals:
    void availabilityChanged(bool available);
    void iconChanged();

private:
    explicit KTalkIntegration(Backend& backend);

    void refreshAvailability();
    void requestIcon();
    void submitStartMeeting(QWidget* parent,
                            const QString& channelId,
                            const QString& rootId,
                            bool callEveryone);
    void handleCustomWebSocketEvent(const QString& eventName,
                                    const QJsonObject& data);
    void showError(QWidget* parent, const QString& message);

    Backend& backend_;
    HTTPConnector httpConnector_;
    QString pluginId_;
    QIcon icon_;
    bool iconRequested_ = false;
};

} // namespace Mattermost
