#include "PostTimelineService.h"

#include <algorithm>
#include <limits>
#include <utility>

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QVariant>

#include "Backend.h"
#include "NetworkRequest.h"
#include "types/BackendChannel.h"
#include "types/BackendPost.h"

namespace Mattermost {
namespace {

struct OrderedPost {
    QString id;
    uint64_t createAt = 0;
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

} // namespace

PostTimelineService& PostTimelineService::instance(Backend& backend)
{
    static QHash<Backend*, QPointer<PostTimelineService>> instances;
    QPointer<PostTimelineService>& service = instances[&backend];
    if (!service) {
        service = new PostTimelineService(backend);
    }
    return *service;
}

PostTimelineService::PostTimelineService(Backend& sourceBackend)
    : QObject(&sourceBackend)
    , backend(sourceBackend)
{
    connect(&httpConnector, &HTTPConnector::onNetworkError,
            &backend, &Backend::onNetworkError);
    connect(&httpConnector, &HTTPConnector::onHttpError,
            &backend, &Backend::onHttpError);
}

void PostTimelineService::coalescedGet(const QString& path, JsonCallback callback)
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

    QPointer<PostTimelineService> guard(this);
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

void PostTimelineService::loadChannelPage(BackendChannel& channel,
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

    QPointer<BackendChannel> channelGuard(&channel);
    coalescedGet(path,
        [channelGuard, callback = std::move(callback)](QVariant status,
                                                       const QJsonDocument& doc) mutable {
            Page result;
            result.success = status.toInt() == QNetworkReply::NoError && doc.isObject();
            if (!result.success || !channelGuard) {
                if (callback) {
                    callback(result);
                }
                return;
            }

            const QJsonObject root = doc.object();
            const QJsonObject posts = root.value(QStringLiteral("posts")).toObject();
            ingest(*channelGuard, posts);
            result.postIds = chronologicalOrder(posts);
            result.prevPostId = root.value(QStringLiteral("prev_post_id")).toString();
            result.nextPostId = root.value(QStringLiteral("next_post_id")).toString();
            if (callback) {
                callback(result);
            }
        });
}

void PostTimelineService::loadChannelBefore(BackendChannel& channel,
                                            const QString& beforePostId,
                                            int perPage,
                                            PageCallback callback)
{
    loadChannelCursor(channel, QStringLiteral("before"), beforePostId,
                      perPage, std::move(callback));
}

void PostTimelineService::loadChannelAfter(BackendChannel& channel,
                                           const QString& afterPostId,
                                           int perPage,
                                           PageCallback callback)
{
    loadChannelCursor(channel, QStringLiteral("after"), afterPostId,
                      perPage, std::move(callback));
}

void PostTimelineService::loadChannelCursor(BackendChannel& channel,
                                            const QString& direction,
                                            const QString& cursorPostId,
                                            int perPage,
                                            PageCallback callback)
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

    QPointer<BackendChannel> channelGuard(&channel);
    coalescedGet(path,
        [channelGuard, cursorPostId, callback = std::move(callback)](
            QVariant status, const QJsonDocument& doc) mutable {
            Page result;
            result.success = status.toInt() == QNetworkReply::NoError && doc.isObject();
            if (!result.success || !channelGuard) {
                if (callback) {
                    callback(result);
                }
                return;
            }

            const QJsonObject root = doc.object();
            const QJsonObject posts = root.value(QStringLiteral("posts")).toObject();
            ingest(*channelGuard, posts);
            result.postIds = chronologicalOrder(posts);
            // Some Mattermost versions include the cursor in context windows.
            // It is an overlap marker, never a new logical row.
            result.postIds.removeAll(cursorPostId);
            result.prevPostId = root.value(QStringLiteral("prev_post_id")).toString();
            result.nextPostId = root.value(QStringLiteral("next_post_id")).toString();
            if (callback) {
                callback(result);
            }
        });
}

void PostTimelineService::loadThreadPage(BackendChannel& channel,
                                         const QString& rootId,
                                         int perPage,
                                         const QString& fromPost,
                                         uint64_t fromCreateAt,
                                         PageCallback callback)
{
    loadThread(channel, rootId, perPage, fromPost, fromCreateAt,
               QStringLiteral("down"), std::move(callback));
}

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

void PostTimelineService::loadThreadTail(BackendChannel& channel,
                                         const QString& rootId,
                                         int perPage,
                                         uint64_t lastReplyAt,
                                         PageCallback callback)
{
    // Server-side `direction=up` sorts newest-first and applies CreateAt <
    // fromCreateAt. Advance the known newest timestamp by one millisecond so
    // replies exactly at last_reply_at are included, then normalize the result
    // back to the client's chronological order.
    const uint64_t afterNewest = lastReplyAt == std::numeric_limits<uint64_t>::max()
        ? lastReplyAt : lastReplyAt + 1;
    loadThread(channel, rootId, perPage, QString(), afterNewest,
               QStringLiteral("up"), std::move(callback));
}

void PostTimelineService::loadThreadFromTime(BackendChannel& channel,
                                             const QString& rootId,
                                             int perPage,
                                             uint64_t fromCreateAt,
                                             PageCallback callback)
{
    loadThread(channel, rootId, perPage, QString(), fromCreateAt,
               QStringLiteral("down"), std::move(callback));
}

void PostTimelineService::loadThread(BackendChannel& channel,
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

    // Mattermost's thread endpoint uses (fromCreateAt, fromPost) as a compound
    // cursor. Supplying fromPost alone is rejected by the server. Callers always
    // reference posts already present in BackendChannel, so recover the required
    // timestamp here rather than ever issuing an invalid request.
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
    QPointer<BackendChannel> channelGuard(&channel);
    coalescedGet(path,
        [channelGuard, rootId, fromPost, initialPage, callback = std::move(callback)](
            QVariant status, const QJsonDocument& doc) mutable {
            Page result;
            result.success = status.toInt() == QNetworkReply::NoError && doc.isObject();
            if (!result.success || !channelGuard) {
                if (callback) {
                    callback(result);
                }
                return;
            }

            const QJsonObject root = doc.object();
            const QJsonObject posts = root.value(QStringLiteral("posts")).toObject();
            ingest(*channelGuard, posts);
            result.postIds = chronologicalOrder(posts, rootId);
            if (!initialPage) {
                result.postIds.removeAll(rootId);
            }
            // Mattermost may include the `fromPost` cursor itself in the next
            // page. It is an overlap marker, not a new logical row; passing it
            // to the source as data would relocate the existing post one index
            // forward and manufacture an artificial gap.
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

QStringList PostTimelineService::chronologicalOrder(const QJsonObject& postsObject,
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

QStringList PostTimelineService::allChronologicalOrder(const QJsonObject& postsObject)
{
    return sortedPostIds(postsObject, {});
}

void PostTimelineService::ingest(BackendChannel& channel, const QJsonObject& postsObject)
{
    // BackendChannel::mergePostContext() is the idempotent ingestion path. It
    // expects Mattermost's newest -> oldest order, while the timeline itself is
    // deliberately represented oldest -> newest.
    const QStringList chronological = allChronologicalOrder(postsObject);
    const int postCount = static_cast<int>(chronological.size());
    QJsonArray newestFirst;
    for (int i = postCount - 1; i >= 0; --i) {
        newestFirst.push_back(chronological.at(i));
    }
    channel.mergePostContext(newestFirst, postsObject);
}

} // namespace Mattermost
