#pragma once

#include <functional>

#include <QtGlobal>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
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
    using ReadCallback = std::function<void(QJsonObject)>;

    PostCacheService();
    explicit PostCacheService(QString databasePath);
    ~PostCacheService();

    PostCacheService(const PostCacheService&) = delete;
    PostCacheService& operator=(const PostCacheService&) = delete;
    PostCacheService(PostCacheService&&) = delete;
    PostCacheService& operator=(PostCacheService&&) = delete;

    /** Queue one channel-open observation for persistent admission policy. */
    void recordChannelOpened(const QString& server,
                             const QString& userId,
                             const QString& channelId,
                             qint64 openedAt);

    /** Queue full raw Mattermost post objects for durable storage. */
    void storePosts(const QString& server,
                    const QString& userId,
                    const QJsonObject& postsObject,
                    quint64 observationSequence);

    /** Queue invalidation of one raw post snapshot. */
    void removePost(const QString& server,
                    const QString& userId,
                    const QString& postId,
                    quint64 observationSequence);

    /** Read one cached post asynchronously; empty object means miss/error. */
    void loadPost(const QString& server,
                  const QString& userId,
                  const QString& postId,
                  ReadCallback callback);

    /** Read newest cached channel roots asynchronously. */
    void loadLatestChannelRoots(const QString& server,
                                const QString& userId,
                                const QString& channelId,
                                int limit,
                                ReadCallback callback);

    /** Read a cached thread root plus newest replies asynchronously. */
    void loadThread(const QString& server,
                    const QString& userId,
                    const QString& channelId,
                    const QString& rootId,
                    int limit,
                    ReadCallback callback);

    /** Persist provenance for an authoritative newest main-channel window. */
    void storeChannelTailWindow(const QString& server,
                                const QString& userId,
                                const QString& channelId,
                                const QStringList& chronologicalPostIds);

    /** Persist provenance for an authoritative newest thread-reply window. */
    void storeThreadTailWindow(const QString& server,
                               const QString& userId,
                               const QString& channelId,
                               const QString& rootId,
                               const QStringList& chronologicalReplyIds);

    /** Read the newest still-complete cached main-channel suffix. */
    void loadChannelTailWindow(const QString& server,
                               const QString& userId,
                               const QString& channelId,
                               int limit,
                               ReadCallback callback);

    /** Read the newest still-complete cached thread-reply suffix. */
    void loadThreadTailWindow(const QString& server,
                              const QString& userId,
                              const QString& channelId,
                              const QString& rootId,
                              int limit,
                              ReadCallback callback);

private:
    QObject callbackContext;
    QThread workerThread;
    PostCacheWorker* worker = nullptr;
};

} // namespace Mattermost
