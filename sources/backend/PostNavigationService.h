/**
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

#include <QObject>
#include <QStringList>

#include "HTTPConnector.h"

namespace Mattermost {

class Backend;
class BackendChannel;

/**
 * Loads channel context around a post that is outside the currently cached
 * timeline. Used by pinned/search-style navigation without paging through the
 * entire channel history.
 */
class PostNavigationService final: public QObject
{
    Q_OBJECT
public:
    struct Context {
        QStringList postIds; // chronological, oldest -> newest
        bool reachedOldest = false;
        bool reachedNewest = false;
        bool success = false;
    };

    using CompletionCallback = std::function<void(bool)>;
    using ContextCallback = std::function<void(const Context&)>;

    static PostNavigationService& instance(Backend& backend);

    // Simple callers only care that the target is now cached.
    void loadAround(BackendChannel& channel,
                    const QString& postId,
                    CompletionCallback callback,
                    bool forceContext = false);

    // Sparse semantic navigation also needs the exact request-local reserve and
    // server edge information so it can materialize a bounded context window.
    void loadAround(BackendChannel& channel,
                    const QString& postId,
                    ContextCallback callback,
                    bool forceContext = false);

private:
    explicit PostNavigationService(Backend& backend);

    Backend& backend;
    HTTPConnector httpConnector;
};

} // namespace Mattermost
