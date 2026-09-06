#include "PostCacheService.h"

#include <algorithm>
#include <memory>
#include <utility>

#include <QDateTime>
#include <QDir>
#include <QHash>
#include <QMetaObject>
#include <QPointer>
#include <QSettings>
#include <QStandardPaths>

#include "PostCacheStore.h"
#include "Settings.h"

namespace Mattermost {
namespace {

constexpr int InvalidationWatermarkPruneThreshold = 4096;
constexpr qint64 InvalidationWatermarkLifetimeMs = 60LL * 60 * 1000;
constexpr qint64 MiB = 1024LL * 1024;

QString defaultDatabasePath()
{
    const QDir cacheRoot(QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
    return cacheRoot.filePath(QStringLiteral("post-cache/posts.sqlite3"));
}

PostCacheStore::Limits configuredLimits()
{
    QSettings settings;
    PostCacheStore::Limits limits;
    limits.maxBytes = std::max<qint64>(
        MiB,
        settings.value(POST_CACHE_DISK_MAX_MB,
                       POST_CACHE_DISK_MAX_MB_DEFAULT).toLongLong() * MiB);
    limits.maxPosts = std::max(
        1,
        settings.value(POST_CACHE_DISK_MAX_POSTS,
                       POST_CACHE_DISK_MAX_POSTS_DEFAULT).toInt());
    limits.maxPostsPerThread = std::max(
        1,
        settings.value(POST_CACHE_DISK_MAX_THREAD_REPLIES,
                       POST_CACHE_DISK_MAX_THREAD_REPLIES_DEFAULT).toInt());
    limits.maxChannelIdleMs = std::max<qint64>(
        60LL * 60 * 1000,
        settings.value(POST_CACHE_DISK_CHANNEL_IDLE_HOURS,
                       POST_CACHE_DISK_CHANNEL_IDLE_HOURS_DEFAULT).toLongLong()
            * 60LL * 60 * 1000);
    limits.maintenanceIntervalMs = std::max(
        60 * 1000,
        settings.value(POST_CACHE_DISK_MAINTENANCE_MINUTES,
                       POST_CACHE_DISK_MAINTENANCE_MINUTES_DEFAULT).toInt()
            * 60 * 1000);
    return limits;
}

QString normalizedServer(QString server)
{
    server = server.trimmed();
    while (server.endsWith(QLatin1Char('/'))) {
        server.chop(1);
    }
    return server;
}

QString watermarkKey(const QString& server, const QString& userId, const QString& postId)
{
    return normalizedServer(server) + QLatin1Char('\x1f') + userId.trimmed()
        + QLatin1Char('\x1f') + postId;
}

} // namespace

class PostCacheWorker final : public QObject
{
public:
    PostCacheWorker(QString path, PostCacheStore::Limits configuredLimits)
        : databasePath(std::move(path))
        , limits(std::move(configuredLimits))
    {
    }

    void recordChannelOpened(const QString& server,
                             const QString& userId,
                             const QString& channelId,
                             qint64 openedAt)
    {
        if (channelId.isEmpty() || !selectAccount(server, userId)) {
            return;
        }
        store->recordChannelOpened(channelId, openedAt);
    }

    void storePosts(const QString& server,
                    const QString& userId,
                    const QJsonObject& postsObject,
                    quint64 observationSequence)
    {
        if (postsObject.isEmpty()) {
            return;
        }

        QJsonObject eligiblePosts;
        for (auto it = postsObject.constBegin(); it != postsObject.constEnd(); ++it) {
            if (!it->isObject()) {
                continue;
            }
            const QJsonObject postObject = it->toObject();
            const QString postId = postObject.value(QStringLiteral("id")).toString(it.key());
            if (postId.isEmpty()) {
                continue;
            }

            const auto watermark = invalidationWatermarks.constFind(
                watermarkKey(server, userId, postId));
            if (watermark != invalidationWatermarks.cend()
                && watermark->observationSequence >= observationSequence) {
                // A delete/reaction event observed after this REST request was
                // dispatched makes its eventual response stale for this post.
                continue;
            }
            eligiblePosts.insert(postId, postObject);
        }

        if (eligiblePosts.isEmpty() || !selectAccount(server, userId)) {
            pruneInvalidationWatermarks();
            return;
        }
        store->storePosts(eligiblePosts);
        pruneInvalidationWatermarks();
    }

    void removePost(const QString& server,
                    const QString& userId,
                    const QString& postId,
                    quint64 observationSequence)
    {
        if (postId.isEmpty()) {
            return;
        }

        const QString key = watermarkKey(server, userId, postId);
        InvalidationWatermark& watermark = invalidationWatermarks[key];
        watermark.observationSequence = std::max(watermark.observationSequence,
                                                 observationSequence);
        watermark.createdAt = QDateTime::currentMSecsSinceEpoch();

        if (selectAccount(server, userId)) {
            store->removePost(postId);
        }
        pruneInvalidationWatermarks();
    }

    QJsonObject loadPost(const QString& server,
                         const QString& userId,
                         const QString& postId)
    {
        if (postId.isEmpty() || !selectAccount(server, userId)) {
            return {};
        }

        // PostCacheStore uses one posts-object shape for all read queries,
        // including a direct lookup: { postId: rawPost }. The service-level
        // direct API intentionally unwraps that container so callers cannot
        // accidentally treat the key wrapper as Mattermost post JSON.
        const QJsonObject posts = store->loadPost(postId);
        const auto post = posts.constFind(postId);
        return post != posts.cend() && post->isObject() ? post->toObject()
                                                        : QJsonObject {};
    }

    QJsonObject loadLatestChannelRoots(const QString& server,
                                       const QString& userId,
                                       const QString& channelId,
                                       int limit)
    {
        if (channelId.isEmpty() || limit <= 0 || !selectAccount(server, userId)) {
            return {};
        }
        return store->loadLatestChannelRoots(channelId, limit);
    }

    QJsonObject loadThread(const QString& server,
                           const QString& userId,
                           const QString& channelId,
                           const QString& rootId,
                           int limit)
    {
        if (channelId.isEmpty() || rootId.isEmpty() || limit <= 0
            || !selectAccount(server, userId)) {
            return {};
        }
        return store->loadThread(channelId, rootId, limit);
    }

    void shutdown()
    {
        if (store) {
            store->maintenance();
            store.reset();
        }
        invalidationWatermarks.clear();
    }

private:
    struct InvalidationWatermark {
        quint64 observationSequence = 0;
        qint64 createdAt = 0;
    };

    bool selectAccount(const QString& server, const QString& userId)
    {
        if (server.trimmed().isEmpty() || userId.trimmed().isEmpty()) {
            return false;
        }
        if (!store) {
            store = std::make_unique<PostCacheStore>(databasePath);
            store->setLimits(limits);
            if (!store->open()) {
                store.reset();
                return false;
            }
        }
        return store->setAccount(server, userId);
    }

    void pruneInvalidationWatermarks()
    {
        if (invalidationWatermarks.size() <= InvalidationWatermarkPruneThreshold) {
            return;
        }

        const qint64 cutoff = QDateTime::currentMSecsSinceEpoch()
            - InvalidationWatermarkLifetimeMs;
        for (auto it = invalidationWatermarks.begin(); it != invalidationWatermarks.end();) {
            if (it->createdAt < cutoff) {
                it = invalidationWatermarks.erase(it);
            } else {
                ++it;
            }
        }
    }

    QString databasePath;
    PostCacheStore::Limits limits;
    std::unique_ptr<PostCacheStore> store;
    QHash<QString, InvalidationWatermark> invalidationWatermarks;
};

PostCacheService::PostCacheService()
    : PostCacheService(defaultDatabasePath())
{
}

PostCacheService::PostCacheService(QString databasePath)
{
    workerThread.setObjectName(QStringLiteral("MattermostPostCache"));
    worker = new PostCacheWorker(std::move(databasePath), configuredLimits());
    worker->moveToThread(&workerThread);
    QObject::connect(&workerThread, &QThread::finished,
                     worker, &QObject::deleteLater);
    workerThread.start();
}

PostCacheService::~PostCacheService()
{
    if (!worker) {
        return;
    }

    if (workerThread.isRunning()) {
        // PostRepository owns this service on the application/backend thread.
        // Blocking here is safe and guarantees that all earlier queued writes
        // have reached SQLite before the worker connection is destroyed.
        Q_ASSERT(QThread::currentThread() != &workerThread);
        PostCacheWorker* const currentWorker = worker;
        QMetaObject::invokeMethod(currentWorker,
                                  [currentWorker] { currentWorker->shutdown(); },
                                  Qt::BlockingQueuedConnection);
        workerThread.quit();
        workerThread.wait();
    }
    worker = nullptr;
}

void PostCacheService::recordChannelOpened(const QString& server,
                                           const QString& userId,
                                           const QString& channelId,
                                           qint64 openedAt)
{
    if (!worker || server.trimmed().isEmpty() || userId.trimmed().isEmpty()
        || channelId.isEmpty()) {
        return;
    }

    PostCacheWorker* const currentWorker = worker;
    QMetaObject::invokeMethod(currentWorker,
                              [currentWorker, server, userId, channelId, openedAt] {
                                  currentWorker->recordChannelOpened(server, userId,
                                                                     channelId, openedAt);
                              },
                              Qt::QueuedConnection);
}

void PostCacheService::storePosts(const QString& server,
                                  const QString& userId,
                                  const QJsonObject& postsObject,
                                  quint64 observationSequence)
{
    if (!worker || server.trimmed().isEmpty() || userId.trimmed().isEmpty()
        || postsObject.isEmpty()) {
        return;
    }

    PostCacheWorker* const currentWorker = worker;
    QMetaObject::invokeMethod(currentWorker,
                              [currentWorker, server, userId, postsObject,
                               observationSequence] {
                                  currentWorker->storePosts(server, userId, postsObject,
                                                            observationSequence);
                              },
                              Qt::QueuedConnection);
}

void PostCacheService::removePost(const QString& server,
                                  const QString& userId,
                                  const QString& postId,
                                  quint64 observationSequence)
{
    if (!worker || server.trimmed().isEmpty() || userId.trimmed().isEmpty()
        || postId.isEmpty()) {
        return;
    }

    PostCacheWorker* const currentWorker = worker;
    QMetaObject::invokeMethod(currentWorker,
                              [currentWorker, server, userId, postId,
                               observationSequence] {
                                  currentWorker->removePost(server, userId, postId,
                                                           observationSequence);
                              },
                              Qt::QueuedConnection);
}

void PostCacheService::loadPost(const QString& server,
                                const QString& userId,
                                const QString& postId,
                                ReadCallback callback)
{
    QPointer<QObject> context(&callbackContext);
    if (!worker || server.trimmed().isEmpty() || userId.trimmed().isEmpty()
        || postId.isEmpty()) {
        if (callback) {
            QMetaObject::invokeMethod(&callbackContext,
                                      [callback = std::move(callback)]() mutable {
                                          callback({});
                                      },
                                      Qt::QueuedConnection);
        }
        return;
    }

    PostCacheWorker* const currentWorker = worker;
    QMetaObject::invokeMethod(currentWorker,
                              [currentWorker, server, userId, postId, context,
                               callback = std::move(callback)]() mutable {
                                  QJsonObject result = currentWorker->loadPost(
                                      server, userId, postId);
                                  if (!context || !callback) {
                                      return;
                                  }
                                  QMetaObject::invokeMethod(
                                      context.data(),
                                      [callback = std::move(callback),
                                       result = std::move(result)]() mutable {
                                          callback(std::move(result));
                                      },
                                      Qt::QueuedConnection);
                              },
                              Qt::QueuedConnection);
}

void PostCacheService::loadLatestChannelRoots(const QString& server,
                                              const QString& userId,
                                              const QString& channelId,
                                              int limit,
                                              ReadCallback callback)
{
    QPointer<QObject> context(&callbackContext);
    if (!worker || server.trimmed().isEmpty() || userId.trimmed().isEmpty()
        || channelId.isEmpty() || limit <= 0) {
        if (callback) {
            QMetaObject::invokeMethod(&callbackContext,
                                      [callback = std::move(callback)]() mutable {
                                          callback({});
                                      },
                                      Qt::QueuedConnection);
        }
        return;
    }

    PostCacheWorker* const currentWorker = worker;
    QMetaObject::invokeMethod(
        currentWorker,
        [currentWorker, server, userId, channelId, limit, context,
         callback = std::move(callback)]() mutable {
            QJsonObject result = currentWorker->loadLatestChannelRoots(
                server, userId, channelId, limit);
            if (!context || !callback) {
                return;
            }
            QMetaObject::invokeMethod(
                context.data(),
                [callback = std::move(callback), result = std::move(result)]() mutable {
                    callback(std::move(result));
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

void PostCacheService::loadThread(const QString& server,
                                  const QString& userId,
                                  const QString& channelId,
                                  const QString& rootId,
                                  int limit,
                                  ReadCallback callback)
{
    QPointer<QObject> context(&callbackContext);
    if (!worker || server.trimmed().isEmpty() || userId.trimmed().isEmpty()
        || channelId.isEmpty() || rootId.isEmpty() || limit <= 0) {
        if (callback) {
            QMetaObject::invokeMethod(&callbackContext,
                                      [callback = std::move(callback)]() mutable {
                                          callback({});
                                      },
                                      Qt::QueuedConnection);
        }
        return;
    }

    PostCacheWorker* const currentWorker = worker;
    QMetaObject::invokeMethod(
        currentWorker,
        [currentWorker, server, userId, channelId, rootId, limit, context,
         callback = std::move(callback)]() mutable {
            QJsonObject result = currentWorker->loadThread(
                server, userId, channelId, rootId, limit);
            if (!context || !callback) {
                return;
            }
            QMetaObject::invokeMethod(
                context.data(),
                [callback = std::move(callback), result = std::move(result)]() mutable {
                    callback(std::move(result));
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

} // namespace Mattermost
