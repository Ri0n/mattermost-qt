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

#include "BackendChannel.h"

#include <algorithm>

#include "backend/Storage.h"
#include "backend/types/BackendUser.h"

namespace Mattermost {

bool BackendChannel::refreshGroupDisplayName()
{
    if (type != groupChannel || members.isEmpty()) {
        return false;
    }

    QStringList names;
    names.reserve(members.size());

    for (auto it = members.cbegin(); it != members.cend(); ++it) {
        const QString& userId = it.key();
        if (storage.loginUser && userId == storage.loginUser->id) {
            continue;
        }

        const BackendUser* user = storage.getUserById(userId);
        if (!user) {
            // Do not replace a complete server username list with a partial
            // display-name list. UserProfileService will call us again once all
            // channel member profiles are available.
            return false;
        }
        names.push_back(user->getDisplayName());
    }

    if (names.isEmpty()) {
        return false;
    }

    std::sort(names.begin(), names.end(), [](const QString& lhs, const QString& rhs) {
        return lhs.compare(rhs, Qt::CaseInsensitive) < 0;
    });

    const QString newDisplayName = names.join(QStringLiteral(", "));
    if (newDisplayName == display_name) {
        return false;
    }

    display_name = newDisplayName;
    emit onUpdated();
    return true;
}

} // namespace Mattermost
