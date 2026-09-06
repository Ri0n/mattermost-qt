#include "PostCacheService.h"

#include <memory>
#include <utility>

#include <QDir>
#include <QMetaObject>
#include <QStandardPaths>

#include "PostCacheStore.h"

namespace Mattermost {
namespace {

QString defaultDatabasePath()
{
    const QDir cacheRoot(QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
    return cacheRoot.filePath(QStringLiteral("post-cache/posts.sqlite3"));
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
                    const QJsonObject& postsObject)
    {
        if (postsObject.isEmpty() || !selectAccount(server, userId)) {
            return;
        }
        store->storePosts(postsObject);
    }

    void removePost(const QString& server,
                    const QString& userId,
                    const QString& postId)
    {
        if (postId.isEmpty() || !selectAccount(server, userId)) {
            return;
        }
        store->removePost(postId);
    }

    void shutdown()
    {
        if (store) {
            store->maintenance();
            store.reset();
        }
    }

private:
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

    QString databasePath;
    std::unique_ptr<PostCacheStore> store;
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
                                  const QJsonObject& postsObject)
{
    if (!worker || server.trimmed().isEmpty() || userId.trimmed().isEmpty()
        || postsObject.isEmpty()) {
        return;
    }

    PostCacheWorker* const currentWorker = worker;
    QMetaObject::invokeMethod(currentWorker,
                              [currentWorker, server, userId, postsObject] {
                                  currentWorker->storePosts(server, userId, postsObject);
                              },
                              Qt::QueuedConnection);
}

void PostCacheService::removePost(const QString& server,
                                  const QString& userId,
                                  const QString& postId)
{
    if (!worker || server.trimmed().isEmpty() || userId.trimmed().isEmpty()
        || postId.isEmpty()) {
        return;
    }

    PostCacheWorker* const currentWorker = worker;
    QMetaObject::invokeMethod(currentWorker,
                              [currentWorker, server, userId, postId] {
                                  currentWorker->removePost(server, userId, postId);
                              },
                              Qt::QueuedConnection);
}

} // namespace Mattermost
