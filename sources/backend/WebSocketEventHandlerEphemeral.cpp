/**
 * @file WebSocketEventHandlerEphemeral.cpp
 * @brief Handling of transient server-side websocket messages.
 */

#include "WebSocketEventHandler.h"

#include <QJsonDocument>

#include "ServerUiService.h"

namespace Mattermost {

void WebSocketEventHandler::handleEphemeralMessage(const QJsonObject& data)
{
    const QJsonObject post = QJsonDocument::fromJson(
        data.value(QStringLiteral("post")).toString().toUtf8()).object();
    const QString message = post.value(QStringLiteral("message")).toString();
    if (message.isEmpty()) {
        return;
    }

    ServerUiService::instance(backend).notifyEphemeralMessage(message);
}

} // namespace Mattermost
