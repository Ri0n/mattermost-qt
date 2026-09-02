/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Mattermost-QT is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Mattermost-QT. if not, see https://www.gnu.org/licenses/.
 */

#pragma once

#include <functional>

#include <QObject>

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
    static PostNavigationService& instance(Backend& backend);

    void loadAround(BackendChannel& channel,
                    const QString& postId,
                    std::function<void(bool)> callback = {});

private:
    explicit PostNavigationService(Backend& backend);

    Backend& backend;
    HTTPConnector httpConnector;
};

} // namespace Mattermost
