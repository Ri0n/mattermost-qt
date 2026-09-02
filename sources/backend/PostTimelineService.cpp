#include "PostTimelineService.h"

#include <algorithm>

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QVariant>

#include "Backend.h"
#include "NetworkRequest.h"
#include "types/BackendChannel.h"

namespace Mattermost {

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
    NetworkRequest request(path);
    httpConnector.get(request, HttpResponseCallback(
        [channelGuard, callback = std::move(callback)](QVariant status, const QJsonDocument& doc) mutable {
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
        }));
}

void PostTimelineService::loadThreadPage(BackendChannel& channel,
                                         const QString& rootId,
                                         int perPage,
                                         const QString& fromPost,
                                         uint64_t fromCreateAt,
                                         PageCallback callback)
{
    loadThread(channel, rootId, perPage, fromPost, fromCreateAt, std::move(callback));
}

void PostTimelineService::loadThreadFromTime(BackendChannel& channel,
                                             const QString& rootId,
                                             int perPage,
                                             uint64_t fromCreateAt,
                                             PageCallback callback)
{
    loadThread(channel, rootId, perPage, QString(), fromCreateAt, std::move(callback));
}

void PostTimelineService::loadThread(BackendChannel& channel,
                                     const QString& rootId,
                                     int perPage,
                                     const QString& fromPost,
                                     uint64_t fromCreateAt,
                                     PageCallback callback)
{
    if (rootId.isEmpty()) {
        Page result;
        if (callback) {
            callback(result);
        }
        return;
    }

    const int safePerPage = std::max(1, perPage);
    QString path = QStringLiteral("posts/") + rootId
        + QStringLiteral("/thread?perPage=") + QString::number(safePerPage)
        + QStringLiteral("&direction=down&skipFetchThreads=true&collapsedThreads=true");
    if (!fromPost.isEmpty()) {
        path += QStringLiteral("&fromPost=") + fromPost;
    }
    if (fromCreateAt != 0) {
        path += QStringLiteral("&fromCreateAt=") + QString::number(fromCreateAt);
    }

    const bool initialPage = fromPost.isEmpty() && fromCreateAt == 0;
    QPointer<BackendChannel> channelGuard(&channel);
    NetworkRequest request(path);
    httpConnector.get(request, HttpResponseCallback(
        [channelGuard, rootId, initialPage, callback = std::move(callback)](
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
            result.prevPostId = root.value(QStringLiteral("prev_post_id")).toString();
            result.nextPostId = root.value(QStringLiteral("next_post_id")).toString();
            result.hasNext = root.value(QStringLiteral("has_next")).toBool();
            if (callback) {
                callback(result);
            }
        }));
}

QStringList PostTimelineService::chronologicalOrder(const QJsonObject& postsObject,
                                                     const QString& rootId)
{
    struct OrderedPost {
        QString id;
        uint64_t createAt = 0;
    };

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

        const QString postRootId = object.value(QStringLiteral("root_id")).toString();
        if (rootId.isEmpty()) {
            if (!postRootId.isEmpty()) {
                continue;
            }
        } else if (id != rootId && postRootId != rootId) {
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

void PostTimelineService::ingest(BackendChannel& channel, const QJsonObject& postsObject)
{
    // BackendChannel::mergePostContext() is the idempotent ingestion path: it
    // deduplicates by post ID and inserts new objects by timestamp without the
    // newest-edge/deletion assumptions of the legacy addPosts() routine.
    QStringList chronological = chronologicalOrder(postsObject, QStringLiteral("*all*"));
    if (chronological.isEmpty()) {
        // The special filter above intentionally matches nothing. Build an
        // unfiltered order here instead; keep filtering only in Page.postIds.
        struct OrderedPost {
            QString id;
            uint64_t createAt = 0;
        };
        QVector<OrderedPost> ordered;
        ordered.reserve(postsObject.size());
        for (auto it = postsObject.constBegin(); it != postsObject.constEnd(); ++it) {
            if (!it->isObject()) {
                continue;
            }
            const QJsonObject object = it->toObject();
            const QString id = object.value(QStringLiteral("id")).toString(it.key());
            if (!id.isEmpty()) {
                ordered.push_back(OrderedPost {
                    id,
                    object.value(QStringLiteral("create_at")).toVariant().toULongLong(),
                });
            }
        }
        std::sort(ordered.begin(), ordered.end(), [](const OrderedPost& lhs, const OrderedPost& rhs) {
            if (lhs.createAt != rhs.createAt) {
                return lhs.createAt < rhs.createAt;
            }
            return lhs.id < rhs.id;
        });
        for (const OrderedPost& post : ordered) {
            chronological.push_back(post.id);
        }
    }

    QJsonArray newestFirst;
    for (auto it = chronological.crbegin(); it != chronological.crend(); ++it) {
        newestFirst.push_back(*it);
    }
    channel.mergePostContext(newestFirst, postsObject);
}

} // namespace Mattermost
