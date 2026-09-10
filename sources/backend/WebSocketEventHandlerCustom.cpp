/**
 * @file WebSocketEventHandlerCustom.cpp
 * @brief Dispatches custom Mattermost WebSocket events to native integrations.
 *
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#include "WebSocketEventHandler.h"

#include "Backend.h"
#include "ServerUiService.h"

namespace Mattermost {

void WebSocketEventHandler::handleCustomEvent(const QString& eventName,
                                              const QJsonObject& data,
                                              const QJsonObject& broadcast)
{
    ServerUiService::instance(backend).notifyCustomWebSocketEvent(
        eventName, data, broadcast);
}

} // namespace Mattermost
