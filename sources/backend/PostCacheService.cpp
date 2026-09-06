#include "PostCacheService.h"

#include <memory>
#include <utility>

#include <QDateTime>
#include <QDir>
#include <QHash>
#include <QMetaObject>
#include <QStandardPaths>

#include "PostCacheStore.h"

namespace Mattermost {
namespace {

constexpr int InvalidationWatermarkPruneThreshold = 4096;
constexpr qint64 InvalidationWatermarkLifetimeMs = 60LL * 60 * 1000;

QString defaultDatabasePath()
{
    const QDir cacheRoot(QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
    return cacheRoot.filePath(QStringLiteral("post-cache/posts.sqlite3"));
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
    return normalizedServer(server) + QChar(0x1f) + userId.trimmed()
        + QChar(0x1f) + postId;
}

} // namespace

class PostCacheWorker final : public QObject
{
public:
    explicit PostCacheWorker(QString path)
        : databasePath(std::move(path))
    {
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
    worker = new PostCacheWorker(std::move(databasePath));
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

} // namespace Mattermost
