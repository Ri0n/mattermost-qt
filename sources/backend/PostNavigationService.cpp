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

#include "PostNavigationService.h"

#include <memory>

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QPointer>
#include <QSignalBlocker>
#include <QVariant>

#include "Backend.h"
#include "NetworkRequest.h"
#include "PostNavigationWindow.h"
#include "types/BackendChannel.h"

namespace Mattermost {
namespace {

// Fetch a reserve large enough for the controller to build a ~31-row window
// even when the target is close to one edge. Only the selected window is
// materialized; the remainder stays in BackendChannel's idempotent cache.
constexpr int ContextFetchPerSide = 30;

struct AroundState {
    QPointer<BackendChannel> channel;
    QString postId;
    QJsonArray beforeOrder;
    QJsonArray afterOrder;
    QJsonObject beforePosts;
    QJsonObject afterPosts;
    QJsonObject targetPost;
    QString beforePrevPostId;
    QString afterNextPostId;
    int pending = 3;
    bool failed = false;
    PostNavigationService::ContextCallback callback;
};

void mergePosts(QJsonObject& destination, const QJsonObject& source)
{
    for (auto it = source.constBegin(); it != source.constEnd(); ++it) {
        destination.insert(it.key(), it.value());
    }
}

} // namespace

PostNavigationService& PostNavigationService::instance(Backend& backend)
{
    static QHash<Backend*, QPointer<PostNavigationService>> instances;
    QPointer<PostNavigationService>& service = instances[&backend];
    if (!service) {
        service = new PostNavigationService(backend);
    }
    return *service;
}

PostNavigationService::PostNavigationService(Backend& sourceBackend)
    : QObject(&sourceBackend)
    , backend(sourceBackend)
{
    connect(&httpConnector, &HTTPConnector::onNetworkError,
            &backend, &Backend::onNetworkError);
    connect(&httpConnector, &HTTPConnector::onHttpError,
            &backend, &Backend::onHttpError);
}

void PostNavigationService::loadAround(BackendChannel& channel,
                                       const QString& postId,
                                       ContextCallback callback,
                                       bool forceContext)
{
    if (postId.isEmpty()) {
        if (callback) {
            callback(Context {});
        }
        return;
    }

    if (!forceContext && channel.postIdToPost.contains(postId)) {
        if (callback) {
            Context result;
            result.success = true;
            result.postIds.push_back(postId);
            callback(result);
        }
        return;
    }

    auto state = std::make_shared<AroundState>();
    state->channel = &channel;
    state->postId = postId;
    state->callback = std::move(callback);

    const auto finishPart = [state](bool success) {
        state->failed = state->failed || !success;
        if (--state->pending != 0) {
            return;
        }

        if (state->failed || !state->channel || state->targetPost.isEmpty()) {
            if (state->callback) {
                state->callback(PostNavigationService::Context {});
            }
            return;
        }

        QJsonObject posts;
        mergePosts(posts, state->afterPosts);
        mergePosts(posts, state->beforePosts);
        posts.insert(state->postId, state->targetPost);

        // Both the backend ingestion order and chronological sparse order come
        // from one tested request-local assembly. Do not derive this window from
        // BackendChannel's wider, potentially disjoint cache.
        const PostNavigationWindow window = buildPostNavigationWindow(
            state->afterOrder, state->postId, state->beforeOrder);

        BackendChannel& currentChannel = *state->channel;
        {
            // Cache population for an explicit semantic jump is not a bulk
            // timeline page. Suppress the generic onNewPosts notification; the
            // caller materializes one bounded context after the reserve is ready.
            const QSignalBlocker blocker(&currentChannel);
            currentChannel.mergePostContext(window.newestFirstOrder, posts);
        }

        if (state->callback) {
            Context result;
            result.success = currentChannel.postIdToPost.contains(state->postId);
            result.reachedOldest = state->beforeOrder.size() < ContextFetchPerSide
                || state->beforePrevPostId.isEmpty();
            result.reachedNewest = state->afterOrder.size() < ContextFetchPerSide
                || state->afterNextPostId.isEmpty();
            result.postIds = window.chronologicalIds;
            state->callback(result);
        }
    };

    auto fetchPostList = [this, state, finishPart](const QString& direction) {
        const QString path = QStringLiteral("channels/") + state->channel->id
            + QStringLiteral("/posts?") + direction + QLatin1Char('=') + state->postId
            + QStringLiteral("&per_page=") + QString::number(ContextFetchPerSide)
            + QStringLiteral("&skipFetchThreads=true&collapsedThreads=true");
        NetworkRequest request(path);
        httpConnector.get(request, HttpResponseCallback(
            [state, finishPart, direction](QVariant status, const QJsonDocument& doc) {
                const bool success = status.toInt() == QNetworkReply::NoError && doc.isObject();
                if (success) {
                    const QJsonObject root = doc.object();
                    if (direction == QLatin1String("before")) {
                        state->beforeOrder = root.value(QStringLiteral("order")).toArray();
                        state->beforePosts = root.value(QStringLiteral("posts")).toObject();
                        state->beforePrevPostId = root.value(QStringLiteral("prev_post_id")).toString();
                    } else {
                        state->afterOrder = root.value(QStringLiteral("order")).toArray();
                        state->afterPosts = root.value(QStringLiteral("posts")).toObject();
                        state->afterNextPostId = root.value(QStringLiteral("next_post_id")).toString();
                    }
                }
                finishPart(success);
            }));
    };

    fetchPostList(QStringLiteral("before"));
    fetchPostList(QStringLiteral("after"));

    NetworkRequest targetRequest(QStringLiteral("posts/") + postId);
    httpConnector.get(targetRequest, HttpResponseCallback(
        [state, finishPart](QVariant status, const QJsonDocument& doc) {
            const bool success = status.toInt() == QNetworkReply::NoError && doc.isObject();
            if (success) {
                state->targetPost = doc.object();
            }
            finishPart(success);
        }));
}

} // namespace Mattermost
