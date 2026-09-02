/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
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

#include "PostNavigationService.h"

#include <memory>

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QPointer>
#include <QSet>
#include <QVariant>

#include "Backend.h"
#include "NetworkRequest.h"
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
    int pending = 3;
    bool failed = false;
    std::function<void(bool)> callback;
};

void mergePosts(QJsonObject& destination, const QJsonObject& source)
{
    for (auto it = source.constBegin(); it != source.constEnd(); ++it) {
        destination.insert(it.key(), it.value());
    }
}

void appendUniqueOrder(QJsonArray& destination, QSet<QString>& seen, const QJsonArray& source)
{
    for (const QJsonValue& value : source) {
        const QString id = value.toString();
        if (id.isEmpty() || seen.contains(id)) {
            continue;
        }
        seen.insert(id);
        destination.push_back(id);
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
                                       std::function<void(bool)> callback,
                                       bool forceContext)
{
    if (postId.isEmpty()) {
        if (callback) {
            callback(false);
        }
        return;
    }

    if (!forceContext && channel.postIdToPost.contains(postId)) {
        if (callback) {
            callback(true);
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
                state->callback(false);
            }
            return;
        }

        QJsonObject posts;
        mergePosts(posts, state->afterPosts);
        mergePosts(posts, state->beforePosts);
        posts.insert(state->postId, state->targetPost);

        // Mattermost PostList order is newest -> oldest. Build the same order
        // as the webapp's getPostsAround(): posts after target, target itself,
        // then posts before it. BackendChannel::mergePostContext() is designed
        // specifically for this arbitrary window and does not apply the
        // newest-edge/deletion assumptions of the legacy reconnect addPosts().
        QJsonArray order;
        QSet<QString> seen;
        appendUniqueOrder(order, seen, state->afterOrder);
        if (!seen.contains(state->postId)) {
            seen.insert(state->postId);
            order.push_back(state->postId);
        }
        appendUniqueOrder(order, seen, state->beforeOrder);

        BackendChannel& currentChannel = *state->channel;
        currentChannel.mergePostContext(order, posts);

        if (state->callback) {
            state->callback(currentChannel.postIdToPost.contains(state->postId));
        }
    };

    auto fetchPostList = [this, state, finishPart](const QString& direction) {
        const QString path = QStringLiteral("channels/") + state->channel->id
            + QStringLiteral("/posts?") + direction + QLatin1Char('=') + state->postId
            + QStringLiteral("&per_page=") + QString::number(ContextFetchPerSide);
        NetworkRequest request(path);
        httpConnector.get(request, HttpResponseCallback(
            [state, finishPart, direction](QVariant status, const QJsonDocument& doc) {
                const bool success = status.toInt() == QNetworkReply::NoError && doc.isObject();
                if (success) {
                    const QJsonObject root = doc.object();
                    if (direction == QLatin1String("before")) {
                        state->beforeOrder = root.value(QStringLiteral("order")).toArray();
                        state->beforePosts = root.value(QStringLiteral("posts")).toObject();
                    } else {
                        state->afterOrder = root.value(QStringLiteral("order")).toArray();
                        state->afterPosts = root.value(QStringLiteral("posts")).toObject();
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
