/**
 * @file ServerUiService.cpp
 * @brief Routes server-originated transient UI requests without coupling the backend to widgets.
 */

#include "ServerUiService.h"

#include <QHash>
#include <QPointer>

#include "Backend.h"

namespace Mattermost {

ServerUiService& ServerUiService::instance(Backend& backend)
{
    static QHash<Backend*, QPointer<ServerUiService>> services;

    if (ServerUiService* existing = services.value(&backend)) {
        return *existing;
    }

    auto* service = new ServerUiService(backend);
    services.insert(&backend, service);
    QObject::connect(&backend, &QObject::destroyed, service, [&backend] {
        services.remove(&backend);
    });
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

} // namespace Mattermost
