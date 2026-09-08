#pragma once

#include <QString>

#include "backend/NetworkRequest.h"
#include "backend/types/BackendPost.h"

namespace Mattermost {

inline QString messagePermalink(const BackendPost& post)
{
    if (post.id.isEmpty()) {
        return QString();
    }

    QString base = NetworkRequest::host();
    if (base.isEmpty()) {
        return QString();
    }
    if (!base.endsWith(QLatin1Char('/'))) {
        base += QLatin1Char('/');
    }

    // Mattermost's redirect permalink is team-independent and therefore also
    // works for DMs/GMs. The server recognizes it as a native permalink when
    // generating metadata.embeds for previews.
    return base + QStringLiteral("_redirect/pl/") + post.id;
}

} // namespace Mattermost
