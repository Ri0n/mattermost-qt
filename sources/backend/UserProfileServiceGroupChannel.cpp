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

#include "UserProfileService.h"

#include "backend/types/BackendChannel.h"

namespace Mattermost {

void UserProfileService::ensureGroupChannelMembers(BackendChannel& channel,
                                                   std::function<void()> callback)
{
    ensureChannelMembers(channel, [&channel, callback = std::move(callback)]() mutable {
        channel.refreshGroupDisplayName();
        if (callback) {
            callback();
        }
    });
}

} // namespace Mattermost
