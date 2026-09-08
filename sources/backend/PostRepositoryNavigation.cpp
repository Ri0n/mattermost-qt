/**
 * @file PostRepositoryNavigation.cpp
 * @brief Ingestion of authoritative post snapshots fetched by navigation.
 *
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#include "PostRepository.h"

#include "Backend.h"
#include "Storage.h"
#include "types/BackendChannel.h"

namespace Mattermost {

bool PostRepository::ingestFetchedPost(const QJsonObject& postObject)
{
    const QString postId = postObject.value(QStringLiteral("id")).toString();
    const QString channelId = postObject.value(QStringLiteral("channel_id")).toString();
    if (postId.isEmpty() || channelId.isEmpty()) {
        return false;
    }

    BackendChannel* channel = backend.getStorage().getChannelById(channelId);
    if (!channel) {
        return false;
    }

    const quint64 observation = cachePostObject(postObject);
    QJsonObject posts;
    posts.insert(postId, postObject);
    ingest(*channel, posts, observation, true);
    return channel->postIdToPost.contains(postId);
}

} // namespace Mattermost
