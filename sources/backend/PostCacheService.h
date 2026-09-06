#pragma once

#include <QJsonObject>
#include <QString>
#include <QThread>

namespace Mattermost {

class PostCacheWorker;

/**
 * Non-blocking front-end for the persistent post cache.
 *
 * SQLite is confined to a dedicated worker thread. Every queued command carries
 * its account identity explicitly, so delayed work cannot leak into a newly
 * selected account after logout/login or server switching.
 */
class PostCacheService final
{
public:
    PostCacheService();
    explicit PostCacheService(QString databasePath);
    ~PostCacheService();

    PostCacheService(const PostCacheService&) = delete;
    PostCacheService& operator=(const PostCacheService&) = delete;
    PostCacheService(PostCacheService&&) = delete;
    PostCacheService& operator=(PostCacheService&&) = delete;

    /** Queue full raw Mattermost post objects for durable storage. */
    void storePosts(const QString& server,
                    const QString& userId,
                    const QJsonObject& postsObject);

    /** Queue invalidation of one raw post snapshot. */
    void removePost(const QString& server,
                    const QString& userId,
                    const QString& postId);

private:
    QThread workerThread;
    PostCacheWorker* worker = nullptr;
};

} // namespace Mattermost
