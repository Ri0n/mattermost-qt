/*
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#include "WebSocketEventHandler.h"

#include "Backend.h"

namespace Mattermost {

void WebSocketEventHandler::handlePreferences(const QVector<QJsonObject>& preferences,
                                              bool deleted)
{
    for (const QJsonObject& preference : preferences) {
        if (preference.value(QStringLiteral("category")).toString()
            != QLatin1String("flagged_post")) {
            continue;
        }

        const QString postId = preference.value(QStringLiteral("name")).toString();
        if (postId.isEmpty()) {
            continue;
        }

        const bool flagged = !deleted
            && preference.value(QStringLiteral("value")).toString()
                .compare(QLatin1String("true"), Qt::CaseInsensitive) == 0;
        emit backend.onFlaggedPostChanged(postId, flagged);
    }
}

void WebSocketEventHandler::handleEvent(const PreferenceChangedEvent& event)
{
    handlePreferences(event.preferences, false);
}

void WebSocketEventHandler::handleEvent(const PreferencesChangedEvent& event)
{
    handlePreferences(event.preferences, false);
}

void WebSocketEventHandler::handleEvent(const PreferencesDeletedEvent& event)
{
    handlePreferences(event.preferences, true);
}

} // namespace Mattermost
