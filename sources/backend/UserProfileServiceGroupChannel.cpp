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

namespace {

constexpr int GroupChannelMembersPageSize = 50;

} // namespace

void UserProfileService::ensureGroupChannelMembers(BackendChannel& channel,
                                                   std::function<void()> callback)
{
    // Group DMs are deliberately small, unlike normal channels. Keep their
    // eager member hydration for display-name reconstruction, but build it on
    // the same paged primitive used by the virtualized channel-member view.
    loadChannelMembersPage(
        channel, 0, GroupChannelMembersPageSize,
        [&channel, callback = std::move(callback)](QStringList) mutable {
            channel.refreshGroupDisplayName();
            if (callback) {
                callback();
            }
        });
}

} // namespace Mattermost
