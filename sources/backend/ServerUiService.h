/**
 * @file ServerUiService.h
 * @brief Routes server-originated transient UI requests without coupling the backend to widgets.
 */

#pragma once

#include <QObject>
#include <QJsonObject>
#include <QString>

namespace Mattermost {

class Backend;

class ServerUiService : public QObject {
    Q_OBJECT
public:
    static ServerUiService& instance(Backend& backend);

    void requestInteractiveDialog(const QJsonObject& dialog,
                                  const QString& url,
                                  const QString& channelId,
                                  const QString& teamId);
    void notifyEphemeralMessage(const QString& message);

signals:
    void interactiveDialogRequested(const QJsonObject& dialog,
                                    const QString& url,
                                    const QString& channelId,
                                    const QString& teamId);
    void ephemeralMessageReceived(const QString& message);

private:
    explicit ServerUiService(Backend& backend);
};

} // namespace Mattermost
