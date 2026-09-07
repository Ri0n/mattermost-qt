/*
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
#include <QSet>
#include <QString>

#include "HTTPConnector.h"

namespace Mattermost {

class Backend;

/** Lazily resolves custom emoji names encountered by message rendering. */
class CustomEmojiService final : public QObject
{
    Q_OBJECT
public:
    static CustomEmojiService& instance(Backend& backend);

    void ensureEmoji(const QString& name);

private:
    explicit CustomEmojiService(Backend& backend);

    void flushPendingNames();
    void lookupNamesIndividually(const QSet<QString>& names);
    void ensureImage(const QString& id, const QString& name);
    static bool isValidCustomEmojiName(const QString& name);

    Backend& _backend;
    HTTPConnector _httpConnector;
    QSet<QString> _pendingNames;
    QSet<QString> _inFlightNames;
    QSet<QString> _missingNames;
    bool _flushScheduled = false;
    bool _batchLookupSupported = true;
};

} // namespace Mattermost
