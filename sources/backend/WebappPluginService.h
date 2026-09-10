/**
 * @file WebappPluginService.h
 * @brief Discovery of active Mattermost webapp plugins.
 *
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

#include <functional>

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVector>

#include "HTTPConnector.h"

namespace Mattermost {

class Backend;

/** Metadata returned by GET /api/v4/plugins/webapp. */
struct WebappPluginManifest {
    QString id;
    QString name;
    QString description;
    QString version;
    QString bundlePath;
    QJsonObject raw;
};

/**
 * Keeps the server-advertised set of active webapp plugins.
 *
 * Native integrations use this as discovery only. We deliberately do not
 * execute arbitrary plugin JavaScript in the Qt client; an integration adapter
 * can instead derive its server paths from the advertised runtime plugin id.
 */
class WebappPluginService final : public QObject
{
    Q_OBJECT
public:
    using LoadCallback = std::function<void(bool)>;
    using BundleCallback = std::function<void(bool, QByteArray)>;

    static WebappPluginService& instance(Backend& backend);

    const QVector<WebappPluginManifest>& plugins() const { return plugins_; }
    const WebappPluginManifest* plugin(const QString& pluginId) const;
    bool hasPlugin(const QString& pluginId) const;

    /** Load manifests once, coalescing concurrent callers. */
    void ensureLoaded(LoadCallback callback = {});
    /** Force a fresh manifest snapshot, coalescing with an in-flight load. */
    void refresh(LoadCallback callback = {});
    /** Cancel requests and forget all state from the current login session. */
    void clear();

    /** Absolute URL of the advertised webapp bundle, or an empty URL. */
    QUrl bundleUrl(const QString& pluginId) const;
    /** Fetch an advertised bundle without executing it. */
    void fetchBundle(const QString& pluginId, BundleCallback callback);

signals:
    void pluginsChanged();

private:
    explicit WebappPluginService(Backend& backend);

    void startLoad();
    void finishLoad(bool success);
    void fetchLoadedBundle(const QString& pluginId, BundleCallback callback);

    HTTPConnector httpConnector_;
    QVector<WebappPluginManifest> plugins_;
    QVector<LoadCallback> loadWaiters_;
    bool loaded_ = false;
    bool loading_ = false;
};

} // namespace Mattermost
