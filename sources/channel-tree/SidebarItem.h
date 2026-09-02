/**
 * @file SidebarItem.h
 * @brief Shared semantic contract for sidebar tree/list rows.
 *
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <QtCore/Qt>

namespace Mattermost {
namespace SidebarItem {

/**
 * Logical row kind shared by Channels, Recent and Attention.
 *
 * The concrete widget is deliberately not part of this contract. Delegates and
 * alternate views should be able to reason about a row solely from model data.
 */
enum Kind {
    Unknown = 0,
    Team,
    Category,
    Channel,
    Thread,
};

/**
 * Shared model roles for sidebar rows.
 *
 * ChannelTypeRole stores BackendChannel::type as an integer. PresenceRole is
 * meaningful only for direct-message rows backed by a concrete user.
 */
enum Role {
    KindRole = Qt::UserRole + 1,
    IdRole,
    TeamIdRole,
    MutedRole,
    MentionedRole,
    PresenceRole,
    UnreadRole,
    LifetimeRole,
    ChannelTypeRole,
    ChannelIdRole,
    ThreadIdRole,
};

} // namespace SidebarItem
} // namespace Mattermost
