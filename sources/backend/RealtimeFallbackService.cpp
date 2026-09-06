#include "RealtimeFallbackService.h"

#include <QHash>
#include <QPointer>
#include <QSet>

#include "Backend.h"
#include "PostRepository.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "log.h"

namespace Mattermost {
namespace {

constexpr int PollIntervalMs = 5000;
constexpr int PollPageSize = 100;

}

RealtimeFallbackService& RealtimeFallbackService::instance(Backend& backend)
{
    static QHash<Backend*, QPointer<RealtimeFallbackService>> instances;
    QPointer<RealtimeFallbackService>& service = instances[&backend];
    if (!service) {
        service = new RealtimeFallbackService(backend);
    }
    return *service;
}

RealtimeFallbackService::RealtimeFallbackService(Backend& sourceBackend)
    : QObject(&sourceBackend)
    , backend(sourceBackend)
{
    pollTimer.setInterval(PollIntervalMs);
    connect(&pollTimer, &QTimer::timeout, this, &RealtimeFallbackService::pollNow);
    connect(&backend, &Backend::onWebSocketDisconnect,
            this, &RealtimeFallbackService::startPolling);
    connect(&backend, &Backend::onWebSocketConnect,
            this, &RealtimeFallbackService::stopPolling);
}

void RealtimeFallbackService::startPolling()
{
    if (polling) {
        return;
    }

    polling = true;
    LOG_DEBUG("WebSocket unavailable; enabling active-channel HTTP polling");
    pollTimer.start();
    pollNow();
}

void RealtimeFallbackService::stopPolling()
{
    if (!polling) {
        return;
    }

    polling = false;
    pollTimer.stop();
    LOG_DEBUG("WebSocket restored; disabling active-channel HTTP polling");
}

void RealtimeFallbackService::pollNow()
{
    if (!polling || requestInFlight) {
        return;
    }

    BackendChannel* channel = backend.getCurrentChannel();
    if (!channel) {
        return;
    }

    QSet<QString> knownPostIds;
    for (auto it = channel->postIdToPost.cbegin(); it != channel->postIdToPost.cend(); ++it) {
        knownPostIds.insert(it.key());
    }

    BackendPost* newestRootPost = nullptr;
    for (BackendPost& post : channel->posts) {
        if (post.hidden || !post.root_id.isEmpty()) {
            continue;
        }
        if (!newestRootPost
            || post.create_at > newestRootPost->create_at
            || (post.create_at == newestRootPost->create_at && post.id > newestRootPost->id)) {
            newestRootPost = &post;
        }
    }

    requestInFlight = true;
    QPointer<RealtimeFallbackService> guard(this);
    QPointer<BackendChannel> channelGuard(channel);
    auto deliver = [guard, channelGuard, knownPostIds](const PostRepository::Page& page) {
        if (!guard) {
            return;
        }
        guard->requestInFlight = false;
        if (!page.success || !channelGuard) {
            return;
        }

        for (const QString& postId : page.postIds) {
            if (knownPostIds.contains(postId)) {
                continue;
            }
            BackendPost* post = channelGuard->postIdToPost.value(postId, nullptr);
            if (!post) {
                continue;
            }

            // PostRepository has already merged and cached the snapshot. Publish
            // only identities that were absent when this poll started, so every
            // polling cycle delivers a newly discovered post at most once.
            emit channelGuard->onNewPost(*post);
            emit guard->backend.onNewPost(*channelGuard, *post);
        }
    };

    auto& repository = PostRepository::instance(backend);
    if (newestRootPost) {
        repository.loadChannelAfter(*channel, newestRootPost->id,
                                    PollPageSize, std::move(deliver));
    } else {
        repository.loadChannelPage(*channel, 0, PollPageSize, std::move(deliver));
    }
}

} // namespace Mattermost
