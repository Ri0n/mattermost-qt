/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#pragma once

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVector>

namespace Mattermost {

namespace PreferencesEventDetail {

inline QVector<QJsonObject> parsePreferences(const QJsonObject& data)
{
    QVector<QJsonObject> result;

    QJsonValue value = data.value(QStringLiteral("preferences"));
    if (value.isString()) {
        const QJsonDocument document = QJsonDocument::fromJson(value.toString().toUtf8());
        value = document.array();
    }

    if (value.isArray()) {
        const QJsonArray array = value.toArray();
        result.reserve(array.size());
        for (const QJsonValue& entry : array) {
            if (entry.isObject()) {
                result.push_back(entry.toObject());
            }
        }
        return result;
    }

    // Older/specialized servers can use a singular preference payload. Accept
    // both an object and a JSON-encoded object without weakening the normal
    // current-server path above.
    value = data.value(QStringLiteral("preference"));
    if (value.isString()) {
        const QJsonDocument document = QJsonDocument::fromJson(value.toString().toUtf8());
        value = document.object();
    }
    if (value.isObject()) {
        result.push_back(value.toObject());
    }
    return result;
}

} // namespace PreferencesEventDetail

class PreferenceChangedEvent
{
public:
    PreferenceChangedEvent(const QJsonObject& data, const QJsonObject&)
        : preferences(PreferencesEventDetail::parsePreferences(data))
    {
    }

    QVector<QJsonObject> preferences;
};

class PreferencesChangedEvent
{
public:
    PreferencesChangedEvent(const QJsonObject& data, const QJsonObject&)
        : preferences(PreferencesEventDetail::parsePreferences(data))
    {
    }

    QVector<QJsonObject> preferences;
};

class PreferencesDeletedEvent
{
public:
    PreferencesDeletedEvent(const QJsonObject& data, const QJsonObject&)
        : preferences(PreferencesEventDetail::parsePreferences(data))
    {
    }

    QVector<QJsonObject> preferences;
};

} // namespace Mattermost
