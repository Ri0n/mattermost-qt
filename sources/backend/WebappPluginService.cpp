/**
 * @file WebappPluginService.cpp
 * @brief Discovery of active Mattermost webapp plugins.
 *
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#include "WebappPluginService.h"

#include <utility>

#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QSet>

#include "Backend.h"
#include "NetworkRequest.h"

namespace Mattermost {
namespace {

Q_LOGGING_CATEGORY(lcWebappPlugins, "mattermost.plugins")

QString siteRelativePath(QString path)
{
    while (path.startsWith(QLatin1Char('/'))) {
        path.remove(0, 1);
    }
    return path;
}

QUrl siteUrl(const QString& path)
{
    QString base = NetworkRequest::host();
    QString suffix = path;
    if (base.isEmpty() || suffix.isEmpty()) {
        return {};
    }

    if (base.endsWith(QLatin1Char('/'))) {
        base.chop(1);
    }
    if (!suffix.startsWith(QLatin1Char('/'))) {
        suffix.prepend(QLatin1Char('/'));
    }
    return QUrl(base + suffix);
}

} // namespace

WebappPluginService& WebappPluginService::instance(Backend& backend)
{
    auto* service = backend.findChild<WebappPluginService*>(
        QString(), Qt::FindDirectChildrenOnly);
    if (!service) {
        service = new WebappPluginService(backend);
    }
    return *service;
}

WebappPluginService::WebappPluginService(Backend& backend)
    : QObject(&backend)
{
}

const WebappPluginManifest* WebappPluginService::plugin(const QString& pluginId) const
{
    for (const WebappPluginManifest& manifest : plugins_) {
        if (manifest.id == pluginId) {
            return &manifest;
        }
    }
    return nullptr;
}

bool WebappPluginService::hasPlugin(const QString& pluginId) const
{
    return plugin(pluginId) != nullptr;
}

void WebappPluginService::ensureLoaded(LoadCallback callback)
{
    if (loaded_ && !loading_) {
        if (callback) {
            callback(true);
        }
        return;
    }

    if (callback) {
        loadWaiters_.push_back(std::move(callback));
    }
    if (!loading_) {
        startLoad();
    }
}

void WebappPluginService::refresh(LoadCallback callback)
{
    if (callback) {
        loadWaiters_.push_back(std::move(callback));
    }
    if (!loading_) {
        startLoad();
    }
}

void WebappPluginService::startLoad()
{
    loading_ = true;

    NetworkRequest request(QStringLiteral("plugins/webapp"));
    httpConnector_.get(request, HttpResponseCallback(
        [this](QVariant status, const QJsonDocument& document) {
            if (status.toInt() != QNetworkReply::NoError || !document.isArray()) {
                qCWarning(lcWebappPlugins)
                    << "Failed to retrieve webapp plugin manifests, status="
                    << status.toInt();
                finishLoad(false);
                return;
            }

            QVector<WebappPluginManifest> manifests;
            QSet<QString> seenIds;
            for (const QJsonValue& value : document.array()) {
                if (!value.isObject()) {
                    continue;
                }

                const QJsonObject object = value.toObject();
                WebappPluginManifest manifest;
                manifest.id = object.value(QStringLiteral("id")).toString();
                if (manifest.id.isEmpty() || seenIds.contains(manifest.id)) {
                    continue;
                }

                manifest.name = object.value(QStringLiteral("name")).toString();
                manifest.description = object.value(QStringLiteral("description")).toString();
                manifest.version = object.value(QStringLiteral("version")).toString();
                manifest.bundlePath = object.value(QStringLiteral("webapp")).toObject()
                    .value(QStringLiteral("bundle_path")).toString();
                manifest.raw = object;

                seenIds.insert(manifest.id);
                manifests.push_back(std::move(manifest));
            }

            plugins_ = std::move(manifests);
            loaded_ = true;

            qCDebug(lcWebappPlugins) << "Discovered" << plugins_.size()
                                    << "active webapp plugins";

            emit pluginsChanged();
            finishLoad(true);
        }));
}

void WebappPluginService::finishLoad(bool success)
{
    loading_ = false;

    QVector<LoadCallback> waiters = std::move(loadWaiters_);
    loadWaiters_.clear();
    for (LoadCallback& callback : waiters) {
        if (callback) {
            callback(success);
        }
    }
}

void WebappPluginService::clear()
{
    httpConnector_.reset();
    plugins_.clear();
    loaded_ = false;
    loading_ = false;

    QVector<LoadCallback> waiters = std::move(loadWaiters_);
    loadWaiters_.clear();
    for (LoadCallback& callback : waiters) {
        if (callback) {
            callback(false);
        }
    }

    emit pluginsChanged();
}

QUrl WebappPluginService::bundleUrl(const QString& pluginId) const
{
    const WebappPluginManifest* manifest = plugin(pluginId);
    return manifest ? siteUrl(manifest->bundlePath) : QUrl();
}

void WebappPluginService::fetchBundle(const QString& pluginId, BundleCallback callback)
{
    if (!callback) {
        return;
    }

    if (!loaded_) {
        ensureLoaded([this, pluginId, callback = std::move(callback)](bool success) mutable {
            if (!success) {
                callback(false, {});
                return;
            }
            fetchLoadedBundle(pluginId, std::move(callback));
        });
        return;
    }

    fetchLoadedBundle(pluginId, std::move(callback));
}

void WebappPluginService::fetchLoadedBundle(const QString& pluginId,
                                            BundleCallback callback)
{
    const WebappPluginManifest* manifest = plugin(pluginId);
    if (!manifest || manifest->bundlePath.isEmpty()) {
        callback(false, {});
        return;
    }

    const QString path = siteRelativePath(manifest->bundlePath);
    NetworkRequest request(QString(), path, true);
    request.setPriority(QNetworkRequest::LowPriority);
    httpConnector_.get(request, HttpResponseCallback(
        [callback = std::move(callback)](QVariant status, QByteArray data) mutable {
            const bool success = status.toInt() == QNetworkReply::NoError;
            callback(success, success ? std::move(data) : QByteArray());
        }));
}

} // namespace Mattermost
