/**
 * @file PostTimelineThreadCursor.cpp
 * @brief Cursor helpers for extending sparse thread seek windows.
 *
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#include "PostTimelineService.h"

#include <utility>

namespace Mattermost {

void PostTimelineService::loadThreadBefore(BackendChannel& channel,
                                           const QString& rootId,
                                           const QString& fromPost,
                                           uint64_t fromCreateAt,
                                           int perPage,
                                           PageCallback callback)
{
    loadThread(channel, rootId, perPage, fromPost, fromCreateAt,
               QStringLiteral("up"), std::move(callback));
}

void PostTimelineService::loadThreadAfter(BackendChannel& channel,
                                          const QString& rootId,
                                          const QString& fromPost,
                                          uint64_t fromCreateAt,
                                          int perPage,
                                          PageCallback callback)
{
    loadThread(channel, rootId, perPage, fromPost, fromCreateAt,
               QStringLiteral("down"), std::move(callback));
}

} // namespace Mattermost
