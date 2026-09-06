#include "PostRepository.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QSet>
#include <QSignalBlocker>
#include <QVariant>

#include "Backend.h"
#include "NetworkRequest.h"
#include "Storage.h"
#include "types/BackendChannel.h"
#include "types/BackendPost.h"

namespace Mattermost {
namespace {

constexpr int ContextFetchPerSide = 30;

struct OrderedPost {
    QString id;
    uint64_t createAt = 0;
};

struct AroundState {
    QPointer<BackendChannel> channel;
    QString postId;
    PostRepository::Page before;
    PostRepository::Page after;
    PostRepository::ContextCallback callback;
    int pending = 3;
    bool failed = false;
};

QStringList sortedPostIds(const QJsonObject& postsObject,
                          const std::function<bool(const QString&, const QString&)>& include)
{
    QVector<OrderedPost> ordered;
    ordered.reserve(postsObject.size());
    for (auto it = postsObject.constBegin(); it != postsObject.constEnd(); ++it) {
        if (!it->isObject()) {
            continue;
        }
        const QJsonObject object = it->toObject();
        const QString id = object.value(QStringLiteral("id")).toString(it.key());
        if (id.isEmpty()) {
            continue;
        }
        const QString rootId = object.value(QStringLiteral("root_id")).toString();
        if (include && !include(id, rootId)) {
            continue;
        }
        ordered.push_back(OrderedPost {
            id,
            object.value(QStringLiteral("create_at")).toVariant().toULongLong(),
        });
    }

    std::sort(ordered.begin(), ordered.end(), [](const OrderedPost& lhs, const OrderedPost& rhs) {
        if (lhs.createAt != rhs.createAt) {
            return lhs.createAt < rhs.createAt;
        }
        return lhs.id < rhs.id;
    });

    QStringList result;
    result.reserve(ordered.size());
    for (const OrderedPost& post : ordered) {
        result.push_back(post.id);
    }
    return result;
}

QStringList combineContext(const QStringList& before,
                           const QString& targetId,
                           const QStringList& after)
{
    QStringList result;
    result.reserve(before.size() + after.size() + 1);
    QSet<QString> seen;

    const auto append = [&result, &seen](const QString& id) {
        if (id.isEmpty() || seen.contains(id)) {
            return;
        }
        seen.insert(id);
        result.push_back(id);
    };

    for (const QString& id : before) {
        append(id);
    }
    append(targetId);
    for (const QString& id : after) {
        append(id);
    }
    return result;
}

} // namespace

PostRepository& PostRepository::instance(Backend& backend)
{
    static QHash<Backend*, QPointer<PostRepository>> instances;
    QPointer<PostRepository>& repository = instances[&backend];
    if (!repository) {
        repository = new PostRepository(backend);
    }
    return *repository;
}

PostRepository::PostRepository(Backend& sourceBackend)
    : QObject(&sourceBackend)
    , backend(sourceBackend)
{
    connect(&httpConnector, &HTTPConnector::onNetworkError,
            &backend, &Backend::onNetworkError);
    connect(&httpConnector, &HTTPConnector::onHttpError,
            &backend, &Backend::onHttpError);
}

PostRepository::CacheAccount PostRepository::currentCacheAccount() const
{
    CacheAccount account;
    const BackendUser* const loginUser = backend.getStorage().loginUser;
    if (!loginUser || loginUser->id.isEmpty()) {
        return account;
    }

    account.server = NetworkRequest::host();
    account.userId = loginUser->id;
    if (!account.isValid()) {
        account = CacheAccount {};
    }
    return account;
}

void PostRepository::cachePosts(const CacheAccount& account,
                                const QJsonObject& postsObject)
{
    if (!account.isValid() || postsObject.isEmpty()) {
        return;
    }
    postCache.storePosts(account.server, account.userId, postsObject);
}

void PostRepository::cachePostObject(const QJsonObject& postObject)
{
    const QString postId = postObject.value(QStringLiteral("id")).toString();
    if (postId.isEmpty()) {
        return;
    }

    QJsonObject posts;
    posts.insert(postId, postObject);
    cachePosts(currentCacheAccount(), posts);
}

void PostRepository::invalidateCachedPost(const QString& postId)
{
    if (postId.isEmpty()) {
        return;
    }
    const CacheAccount account = currentCacheAccount();
    if (!account.isValid()) {
        return;
    }
    postCache.removePost(account.server, account.userId, postId);
}

void PostRepository::coalescedGet(const QString& path, JsonCallback callback)
{
    if (path.isEmpty()) {
        if (callback) {
            callback(QVariant::fromValue(static_cast<int>(QNetworkReply::ProtocolInvalidOperationError)),
                     QJsonDocument());
        }
        return;
    }

    auto existing = inFlightGets.find(path);
    if (existing != inFlightGets.end()) {
        if (callback) {
            existing->push_back(std::move(callback));
        }
        return;
    }

    QList<JsonCallback> callbacks;
    if (callback) {
        callbacks.push_back(std::move(callback));
    }
    inFlightGets.insert(path, std::move(callbacks));

    QPointer<PostRepository> guard(this);
    NetworkRequest request(path);
    httpConnector.get(request, HttpResponseCallback(
        [guard, path](QVariant status, const QJsonDocument& doc) mutable {
            if (!guard) {
                return;
            }

            // Remove before fan-out: a callback may synchronously request the
            // same URL again and that must start a fresh request rather than join
            // an already completed transaction.
            QList<JsonCallback> waiting = guard->inFlightGets.take(path);
            for (JsonCallback& cb : waiting) {
                if (cb) {
                    cb(status, doc);
                }
            }
        }));
}

void PostRepository::loadPost(const QString& postId, PostCallback callback)
{
    if (postId.isEmpty()) {
        if (callback) {
            callback(PostResult {});
        }
        return;
    }

    const CacheAccount cacheAccount = currentCacheAccount();
    QPointer<PostRepository> guard(this);
    coalescedGet(QStringLiteral("posts/") + postId,
        [guard, postId, cacheAccount, callback = std::move(callback)](
            QVariant status, const QJsonDocument& doc) mutable {
            PostResult result;
            result.postId = postId;
            if (!guard || status.toInt() != QNetworkReply::NoError || !doc.isObject()) {
                if (callback) {
                    callback(result);
                }
                return;
            }

            const QJsonObject postObject = doc.object();
            result.channelId = postObject.value(QStringLiteral("channel_id")).toString();
            result.rootId = postObject.value(QStringLiteral("root_id")).toString();

            QJsonObject posts;
            posts.insert(postId, postObject);
            guard->cachePosts(cacheAccount, posts);

            BackendChannel* channel = guard->backend.getStorage().getChannelById(result.channelId);
            if (channel) {
                ingest(*channel, posts, true);
                result.success = channel->postIdToPost.contains(postId);
            }

            if (callback) {
                callback(result);
            }
        });
}

void PostRepository::loadChannelPage(BackendChannel& channel,
                                     int page,
                                     int perPage,
                                     PageCallback callback)
{
    const int safePage = std::max(0, page);
    const int safePerPage = std::max(1, perPage);
    const QString path = QStringLiteral("channels/") + channel.id
        + QStringLiteral("/posts?page=") + QString::number(safePage)
        + QStringLiteral("&per_page=") + QString::number(safePerPage)
        + QStringLiteral("&skipFetchThreads=true&collapsedThreads=true");

    const CacheAccount cacheAccount = currentCacheAccount();
    QPointer<PostRepository> guard(this);
    QPointer<BackendChannel> channelGuard(&channel);
    coalescedGet(path,
        [guard, channelGuard, cacheAccount, callback = std::move(callback)](
            QVariant status, const QJsonDocument& doc) mutable {
            Page result;
            result.success = status.toInt() == QNetworkReply::NoError && doc.isObject();
            if (!result.success || !guard) {
                if (callback) {
                    callback(result);
                }
                return;
            }

            const QJsonObject root = doc.object();
            const QJsonObject posts = root.value(QStringLiteral("posts")).toObject();
            guard->cachePosts(cacheAccount, posts);
            if (!channelGuard) {
                if (callback) {
                    callback(result);
                }
                return;
            }

            ingest(*channelGuard, posts);
            result.postIds = chronologicalOrder(posts);
            result.prevPostId = root.value(QStringLiteral("prev_post_id")).toString();
            result.nextPostId = root.value(QStringLiteral("next_post_id")).toString();
            if (callback) {
                callback(result);
            }
        });
}

void PostRepository::loadChannelBefore(BackendChannel& channel,
                                       const QString& beforePostId,
                                       int perPage,
                                       PageCallback callback)
{
    loadChannelCursor(channel, QStringLiteral("before"), beforePostId,
                      perPage, std::move(callback));
}

void PostRepository::loadChannelAfter(BackendChannel& channel,
                                      const QString& afterPostId,
                                      int perPage,
                                      PageCallback callback)
{
    loadChannelCursor(channel, QStringLiteral("after"), afterPostId,
                      perPage, std::move(callback));
}

void PostRepository::loadChannelCursor(BackendChannel& channel,
                                       const QString& direction,
                                       const QString& cursorPostId,
                                       int perPage,
                                       PageCallback callback,
                                       bool quietIngest)
{
    if (cursorPostId.isEmpty()
        || (direction != QLatin1String("before") && direction != QLatin1String("after"))) {
        Page result;
        if (callback) {
            callback(result);
        }
        return;
    }

    const int safePerPage = std::max(1, perPage);
    const QString path = QStringLiteral("channels/") + channel.id
        + QStringLiteral("/posts?") + direction + QLatin1Char('=') + cursorPostId
        + QStringLiteral("&per_page=") + QString::number(safePerPage)
        + QStringLiteral("&skipFetchThreads=true&collapsedThreads=true");

    const CacheAccount cacheAccount = currentCacheAccount();
    QPointer<PostRepository> guard(this);
    QPointer<BackendChannel> channelGuard(&channel);
    coalescedGet(path,
        [guard, channelGuard, cacheAccount, cursorPostId, quietIngest,
         callback = std::move(callback)](QVariant status, const QJsonDocument& doc) mutable {
            Page result;
            result.success = status.toInt() == QNetworkReply::NoError && doc.isObject();
            if (!result.success || !guard) {
                if (callback) {
                    callback(result);
                }
                return;
            }

            const QJsonObject root = doc.object();
            const QJsonObject posts = root.value(QStringLiteral("posts")).toObject();
            guard->cachePosts(cacheAccount, posts);
            if (!channelGuard) {
                if (callback) {
                    callback(result);
                }
                return;
            }

            ingest(*channelGuard, posts, quietIngest);
            result.postIds = chronologicalOrder(posts);
            result.postIds.removeAll(cursorPostId);
            result.prevPostId = root.value(QStringLiteral("prev_post_id")).toString();
            result.nextPostId = root.value(QStringLiteral("next_post_id")).toString();
            if (callback) {
                callback(result);
            }
        });
}

void PostRepository::loadChannelAround(BackendChannel& channel,
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

        if (state->failed || !state->channel
            || !state->channel->postIdToPost.contains(state->postId)) {
            if (state->callback) {
                state->callback(PostRepository::Context {});
            }
            return;
        }

        Context result;
        result.success = true;
        result.reachedOldest = state->before.postIds.size() < ContextFetchPerSide
            || state->before.prevPostId.isEmpty();
        result.reachedNewest = state->after.postIds.size() < ContextFetchPerSide
            || state->after.nextPostId.isEmpty();
        result.postIds = combineContext(state->before.postIds,
                                        state->postId,
                                        state->after.postIds);
        if (state->callback) {
            state->callback(result);
        }
    };

    loadChannelCursor(channel, QStringLiteral("before"), postId, ContextFetchPerSide,
        [state, finishPart](const Page& page) {
            state->before = page;
            finishPart(page.success);
        },
        true);

    loadChannelCursor(channel, QStringLiteral("after"), postId, ContextFetchPerSide,
        [state, finishPart](const Page& page) {
            state->after = page;
            finishPart(page.success);
        },
        true);

    loadPost(postId,
        [finishPart](const PostResult& result) {
            finishPart(result.success);
        });
}

void PostRepository::loadThreadPage(BackendChannel& channel,
                                    const QString& rootId,
                                    int perPage,
                                    const QString& fromPost,
                                    uint64_t fromCreateAt,
                                    PageCallback callback)
{
    loadThread(channel, rootId, perPage, fromPost, fromCreateAt,
               QStringLiteral("down"), std::move(callback));
}

void PostRepository::loadThreadBefore(BackendChannel& channel,
                                      const QString& rootId,
                                      const QString& fromPost,
                                      uint64_t fromCreateAt,
                                      int perPage,
                                      PageCallback callback)
{
    loadThread(channel, rootId, perPage, fromPost, fromCreateAt,
               QStringLiteral("up"), std::move(callback));
}

void PostRepository::loadThreadAfter(BackendChannel& channel,
                                     const QString& rootId,
                                     const QString& fromPost,
                                     uint64_t fromCreateAt,
                                     int perPage,
                                     PageCallback callback)
{
    loadThread(channel, rootId, perPage, fromPost, fromCreateAt,
               QStringLiteral("down"), std::move(callback));
}

void PostRepository::loadThreadTail(BackendChannel& channel,
                                    const QString& rootId,
                                    int perPage,
                                    uint64_t lastReplyAt,
                                    PageCallback callback)
{
    const uint64_t afterNewest = lastReplyAt == std::numeric_limits<uint64_t>::max()
        ? lastReplyAt : lastReplyAt + 1;
    loadThread(channel, rootId, perPage, QString(), afterNewest,
               QStringLiteral("up"), std::move(callback));
}

void PostRepository::loadThreadFromTime(BackendChannel& channel,
                                        const QString& rootId,
                                        int perPage,
                                        uint64_t fromCreateAt,
                                        PageCallback callback)
{
    loadThread(channel, rootId, perPage, QString(), fromCreateAt,
               QStringLiteral("down"), std::move(callback));
}

void PostRepository::loadThread(BackendChannel& channel,
                                const QString& rootId,
                                int perPage,
                                const QString& fromPost,
                                uint64_t fromCreateAt,
                                const QString& direction,
                                PageCallback callback)
{
    if (rootId.isEmpty()) {
        Page result;
        if (callback) {
            callback(result);
        }
        return;
    }

    uint64_t effectiveFromCreateAt = fromCreateAt;
    if (!fromPost.isEmpty() && effectiveFromCreateAt == 0) {
        BackendPost* cursor = channel.postIdToPost.value(fromPost, nullptr);
        if (!cursor) {
            Page result;
            if (callback) {
                callback(result);
            }
            return;
        }
        effectiveFromCreateAt = cursor->create_at;
    }

    const int safePerPage = std::max(1, perPage);
    const QString safeDirection = direction == QLatin1String("up")
        ? QStringLiteral("up") : QStringLiteral("down");
    QString path = QStringLiteral("posts/") + rootId
        + QStringLiteral("/thread?perPage=") + QString::number(safePerPage)
        + QStringLiteral("&direction=") + safeDirection
        + QStringLiteral("&skipFetchThreads=true&collapsedThreads=true");
    if (!fromPost.isEmpty()) {
        path += QStringLiteral("&fromPost=") + fromPost;
    }
    if (effectiveFromCreateAt != 0) {
        path += QStringLiteral("&fromCreateAt=") + QString::number(effectiveFromCreateAt);
    }

    const bool initialPage = safeDirection == QLatin1String("down")
        && fromPost.isEmpty() && effectiveFromCreateAt == 0;
    const CacheAccount cacheAccount = currentCacheAccount();
    QPointer<PostRepository> guard(this);
    QPointer<BackendChannel> channelGuard(&channel);
    coalescedGet(path,
        [guard, channelGuard, cacheAccount, rootId, fromPost, initialPage,
         callback = std::move(callback)](QVariant status, const QJsonDocument& doc) mutable {
            Page result;
            result.success = status.toInt() == QNetworkReply::NoError && doc.isObject();
            if (!result.success || !guard) {
                if (callback) {
                    callback(result);
                }
                return;
            }

            const QJsonObject root = doc.object();
            const QJsonObject posts = root.value(QStringLiteral("posts")).toObject();
            guard->cachePosts(cacheAccount, posts);
            if (!channelGuard) {
                if (callback) {
                    callback(result);
                }
                return;
            }

            ingest(*channelGuard, posts);
            result.postIds = chronologicalOrder(posts, rootId);
            if (!initialPage) {
                result.postIds.removeAll(rootId);
            }
            if (!fromPost.isEmpty()) {
                result.postIds.removeAll(fromPost);
            }
            result.prevPostId = root.value(QStringLiteral("prev_post_id")).toString();
            result.nextPostId = root.value(QStringLiteral("next_post_id")).toString();
            result.hasNext = root.value(QStringLiteral("has_next")).toBool();
            if (callback) {
                callback(result);
            }
        });
}

QStringList PostRepository::chronologicalOrder(const QJsonObject& postsObject,
                                                const QString& rootId)
{
    if (rootId.isEmpty()) {
        return sortedPostIds(postsObject, [](const QString&, const QString& postRootId) {
            return postRootId.isEmpty();
        });
    }

    return sortedPostIds(postsObject, [&rootId](const QString& id, const QString& postRootId) {
        return id == rootId || postRootId == rootId;
    });
}

QStringList PostRepository::allChronologicalOrder(const QJsonObject& postsObject)
{
    return sortedPostIds(postsObject, {});
}

void PostRepository::ingest(BackendChannel& channel,
                            const QJsonObject& postsObject,
                            bool quiet)
{
    const QStringList chronological = allChronologicalOrder(postsObject);
    QJsonArray newestFirst;
    for (int i = chronological.size() - 1; i >= 0; --i) {
        newestFirst.push_back(chronological.at(i));
    }

    if (quiet) {
        const QSignalBlocker blocker(&channel);
        channel.mergePostContext(newestFirst, postsObject);
    } else {
        channel.mergePostContext(newestFirst, postsObject);
    }
}

} // namespace Mattermost
