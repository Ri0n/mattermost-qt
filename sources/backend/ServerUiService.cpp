/**
 * @file ServerUiService.cpp
 * @brief Routes server-originated transient UI requests without coupling the backend to widgets.
 */

#include "ServerUiService.h"

#include "Backend.h"

namespace Mattermost {

ServerUiService& ServerUiService::instance(Backend& backend)
{
    auto* service = backend.findChild<ServerUiService*>(
        QString(), Qt::FindDirectChildrenOnly);
    if (!service) {
        service = new ServerUiService(backend);
    }
    return *service;
}

ServerUiService::ServerUiService(Backend& backend)
    : QObject(&backend)
{
}

void ServerUiService::requestInteractiveDialog(const QJsonObject& dialog,
                                               const QString& url,
                                               const QString& channelId,
                                               const QString& teamId)
{
    emit interactiveDialogRequested(dialog, url, channelId, teamId);
}

void ServerUiService::notifyEphemeralMessage(const QString& message)
{
    if (!message.isEmpty()) {
        emit ephemeralMessageReceived(message);
    }
}

void ServerUiService::notifyCustomWebSocketEvent(const QString& eventName,
                                                  const QJsonObject& data,
                                                  const QJsonObject& broadcast)
{
    if (!eventName.isEmpty()) {
        emit customWebSocketEventReceived(eventName, data, broadcast);
    }
}

} // namespace Mattermost
