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
#include <QNetworkReply>
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
namespace {

constexpr int MaxNamesPerBatch = 200;

bool isUnsupportedBatchStatus(int status)
{
    return status == 404 || status == 405 || status == 501;
}

} // namespace

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
    connect(&EmojiRegistryNotifier::instance(),
            &EmojiRegistryNotifier::customEmojiRequested,
            this, &CustomEmojiService::ensureEmoji);

    // Requests owned by this best-effort background resolver are deliberately
    // not forwarded to Backend::onNetworkError. A perfectly ordinary literal
    // such as :not_an_emoji: may resolve to HTTP 404 and must remain silent.
    // Clear negative/transient state on reconnect so new server-side emoji and
    // cancelled requests become eligible for lookup again.
    connect(&_backend, &Backend::onWebSocketConnect, this, [this] {
        _httpConnector.reset();
        _pendingNames.clear();
        _inFlightNames.clear();
        _missingNames.clear();
        _flushScheduled = false;
        _batchLookupSupported = true;
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

    QSet<QString> requested;
    auto it = _pendingNames.begin();
    while (it != _pendingNames.end() && requested.size() < MaxNamesPerBatch) {
        requested.insert(*it);
        it = _pendingNames.erase(it);
    }
    _inFlightNames.unite(requested);

    if (!_pendingNames.isEmpty()) {
        _flushScheduled = true;
        QTimer::singleShot(0, this, [this] {
            flushPendingNames();
        });
    }

    if (!_batchLookupSupported) {
        lookupNamesIndividually(requested);
        return;
    }

    QJsonArray names;
    for (const QString& name : requested) {
        names.push_back(name);
    }

    NetworkRequest request(QStringLiteral("emoji/names"));
    _httpConnector.post(request, QByteArrayCreator(names),
                        HttpResponseCallback(
        [this, requested](const QJsonDocument& doc, const QNetworkReply& reply) {
            if (reply.error() != QNetworkReply::NoError) {
                const int httpStatus = reply.attribute(
                    QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (isUnsupportedBatchStatus(httpStatus)) {
                    // POST /emoji/names was added in Mattermost 9.2. Fall back
                    // to the per-name endpoint available since 4.7 and remember
                    // that decision for the rest of this connection.
                    _batchLookupSupported = false;
                    lookupNamesIndividually(requested);
                    return;
                }

                for (const QString& name : requested) {
                    _inFlightNames.remove(name);
                }
                return;
            }

            QSet<QString> found;
            for (const QJsonValue& value : doc.array()) {
                const QJsonObject object = value.toObject();
                const QString id = object.value(QStringLiteral("id")).toString();
                const QString name = object.value(QStringLiteral("name")).toString();
                if (id.isEmpty() || name.isEmpty() || !requested.contains(name)) {
                    continue;
                }

                found.insert(name);
                // Keep the name in-flight until its cached or downloaded image
                // has actually been registered in EmojiInfo.
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

void CustomEmojiService::lookupNamesIndividually(const QSet<QString>& names)
{
    for (const QString& requestedName : names) {
        NetworkRequest request(QStringLiteral("emoji/name/") + requestedName);
        _httpConnector.get(request, HttpResponseCallback(
            [this, requestedName](const QJsonDocument& doc, const QNetworkReply& reply) {
                if (reply.error() != QNetworkReply::NoError) {
                    const int httpStatus = reply.attribute(
                        QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    _inFlightNames.remove(requestedName);
                    if (httpStatus == 404) {
                        _missingNames.insert(requestedName);
                    }
                    return;
                }

                const QJsonObject object = doc.object();
                const QString id = object.value(QStringLiteral("id")).toString();
                const QString name = object.value(QStringLiteral("name")).toString();
                if (id.isEmpty() || name.isEmpty()) {
                    _inFlightNames.remove(requestedName);
                    return;
                }

                ensureImage(id, name);
            }));
    }
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
        [this, name, filePath](QVariant status, QByteArray data) {
            if (status.toInt() != QNetworkReply::NoError || data.isEmpty()) {
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
