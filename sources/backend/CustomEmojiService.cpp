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

#include "CustomEmojiService.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

#include "Backend.h"
#include "NetworkRequest.h"
#include "QByteArrayCreator.h"
#include "emoji/EmojiInfo.h"
#include "emoji/EmojiRegistryNotifier.h"

namespace Mattermost {

CustomEmojiService& CustomEmojiService::instance(Backend& backend)
{
    static QHash<Backend*, QPointer<CustomEmojiService>> instances;
    QPointer<CustomEmojiService>& service = instances[&backend];
    if (!service) {
        service = new CustomEmojiService(backend);
    }
    return *service;
}

CustomEmojiService::CustomEmojiService(Backend& backend)
    : QObject(&backend)
    , _backend(backend)
{
    connect(&_httpConnector, &HTTPConnector::onNetworkError,
            &_backend, &Backend::onNetworkError);
    connect(&_httpConnector, &HTTPConnector::onHttpError,
            &_backend, &Backend::onHttpError);

    connect(&EmojiRegistryNotifier::instance(),
            &EmojiRegistryNotifier::customEmojiRequested,
            this, &CustomEmojiService::ensureEmoji);

    // HTTPConnector requests can be cancelled during reconnect. Make names
    // eligible for another lookup instead of leaving a cancelled request stuck
    // in the in-flight/missing sets forever. This also lets newly-created custom
    // emoji become discoverable after a reconnect.
    connect(&_backend, &Backend::onWebSocketConnect, this, [this] {
        _pendingNames.clear();
        _inFlightNames.clear();
        _missingNames.clear();
        _flushScheduled = false;
    });
}

bool CustomEmojiService::isValidCustomEmojiName(const QString& name)
{
    static const QRegularExpression expression(
        QStringLiteral(R"(^[A-Za-z0-9_+\-]+$)"));
    return !name.isEmpty() && expression.match(name).hasMatch();
}

void CustomEmojiService::ensureEmoji(const QString& name)
{
    // EmojiInfo emits customEmojiRequested only after its local lookup misses,
    // so looking it up again here would recurse back into this slot.
    if (!isValidCustomEmojiName(name)
        || _pendingNames.contains(name)
        || _inFlightNames.contains(name)
        || _missingNames.contains(name)) {
        return;
    }

    _pendingNames.insert(name);
    if (_flushScheduled) {
        return;
    }

    _flushScheduled = true;
    QTimer::singleShot(0, this, [this] {
        flushPendingNames();
    });
}

void CustomEmojiService::flushPendingNames()
{
    _flushScheduled = false;
    if (_pendingNames.isEmpty()) {
        return;
    }

    const QSet<QString> requested = _pendingNames;
    _pendingNames.clear();
    _inFlightNames.unite(requested);

    QJsonArray names;
    for (const QString& name : requested) {
        names.push_back(name);
    }

    NetworkRequest request(QStringLiteral("emoji/names"));
    _httpConnector.post(request, QByteArrayCreator(names),
                        HttpResponseCallback([this, requested](const QJsonDocument& doc) {
        QSet<QString> found;
        for (const QJsonValue& value : doc.array()) {
            const QJsonObject object = value.toObject();
            const QString id = object.value(QStringLiteral("id")).toString();
            const QString name = object.value(QStringLiteral("name")).toString();
            if (id.isEmpty() || name.isEmpty() || !requested.contains(name)) {
                continue;
            }

            found.insert(name);
            // Keep the name in-flight until its cached or downloaded image has
            // actually been registered in EmojiInfo. Otherwise another render
            // pass can issue a duplicate metadata/image request in this gap.
            ensureImage(id, name);
        }

        for (const QString& name : requested) {
            if (found.contains(name)) {
                continue;
            }
            _inFlightNames.remove(name);
            _missingNames.insert(name);
        }
    }));
}

void CustomEmojiService::ensureImage(const QString& id, const QString& name)
{
    QDir cacheDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
    QDir emojiDir(cacheDir.filePath(QStringLiteral("custom-emoji")));
    if (!emojiDir.exists() && !emojiDir.mkpath(QStringLiteral("."))) {
        _inFlightNames.remove(name);
        return;
    }

    // Keep the existing cache layout. The .gif suffix is historical; Qt image
    // readers identify PNG/JPEG/GIF data by content when QTextDocument loads it.
    const QString filePath = emojiDir.filePath(id + QStringLiteral(".gif"));
    const QFileInfo cached(filePath);
    if (cached.exists() && cached.isFile() && cached.size() > 0) {
        EmojiInfo::addCustomEmoji(name, filePath);
        _inFlightNames.remove(name);
        return;
    }

    NetworkRequest request(QStringLiteral("emoji/") + id + QStringLiteral("/image"));
    request.setPriority(QNetworkRequest::LowPriority);
    request.setAttribute(QNetworkRequest::BackgroundRequestAttribute, true);

    _httpConnector.get(request, HttpResponseCallback(
        [this, name, filePath](QVariant, QByteArray data) {
            if (data.isEmpty()) {
                _inFlightNames.remove(name);
                return;
            }

            QFile file(filePath);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                _inFlightNames.remove(name);
                return;
            }
            if (file.write(data) != data.size()) {
                file.close();
                QFile::remove(filePath);
                _inFlightNames.remove(name);
                return;
            }
            file.close();

            EmojiInfo::addCustomEmoji(name, filePath);
            _inFlightNames.remove(name);
        }));
}

} // namespace Mattermost
