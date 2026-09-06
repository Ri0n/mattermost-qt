/**
 * @file PostRepository.h
 * @brief Centralized Mattermost post retrieval and cache ingestion.
 *
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#pragma once

#include <cstdint>
#include <functional>

#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QVariant>

#include "HTTPConnector.h"
#include "PostCacheService.h"

namespace Mattermost {

class Backend;
class BackendChannel;

/**
 * Single active owner of post REST retrieval.
 *
 * Sources own logical index mapping, navigation owns semantic intent, and
 * BackendChannel owns the cached model. This repository owns Mattermost REST
 * pagination details, response normalization, request coalescing and cache
 * ingestion shared by those callers.
 */
class PostRepository final : public QObject
{
    Q_OBJECT
public:
    struct Page {
        QStringList postIds; // chronological: oldest -> newest
        QString prevPostId;
        QString nextPostId;
        bool hasNext = false;
        bool success = false;
    };

    struct Context {
        QStringList postIds; // chronological: oldest -> newest
        bool reachedOldest = false;
        bool reachedNewest = false;
        bool success = false;
    };

    struct PostResult {
        QString postId;
        QString channelId;
        QString rootId;
        bool success = false;
    };

    using PageCallback = std::function<void(const Page&)>;
    using ContextCallback = std::function<void(const Context&)>;
    using PostCallback = std::function<void(const PostResult&)>;

    static PostRepository& instance(Backend& backend);

    /** Fetch one post by id and quietly merge it into its known channel cache. */
    void loadPost(const QString& postId, PostCallback callback);

    /** Fetch an absolute main-channel page. Replies are deliberately excluded. */
    void loadChannelPage(BackendChannel& channel,
                         int page,
                         int perPage,
                         PageCallback callback);

    /** Extend an arbitrary channel window toward older posts from a known post. */
    void loadChannelBefore(BackendChannel& channel,
                           const QString& beforePostId,
                           int perPage,
                           PageCallback callback);

    /** Extend an arbitrary channel window toward newer posts from a known post. */
    void loadChannelAfter(BackendChannel& channel,
                          const QString& afterPostId,
                          int perPage,
                          PageCallback callback);

    /** Load a bounded channel context around a semantic target post. */
    void loadChannelAround(BackendChannel& channel,
                           const QString& postId,
                           ContextCallback callback,
                           bool forceContext = false);

    /**
     * Fetch a thread page in the forward/down direction. Mattermost treats
     * (fromCreateAt, fromPost) as a compound cursor. When fromPost is supplied
     * with fromCreateAt == 0, the repository resolves the cached post's create_at
     * before issuing the request; it never sends a bare fromPost cursor.
     */
    void loadThreadPage(BackendChannel& channel,
                        const QString& rootId,
                        int perPage,
                        const QString& fromPost,
                        uint64_t fromCreateAt,
                        PageCallback callback);

    /** Fetch up to perPage replies immediately older than a known thread post. */
    void loadThreadBefore(BackendChannel& channel,
                          const QString& rootId,
                          const QString& fromPost,
                          uint64_t fromCreateAt,
                          int perPage,
                          PageCallback callback);

    /** Fetch up to perPage replies immediately newer than a known thread post. */
    void loadThreadAfter(BackendChannel& channel,
                         const QString& rootId,
                         const QString& fromPost,
                         uint64_t fromCreateAt,
                         int perPage,
                         PageCallback callback);

    /** Fetch the newest replies in a thread and normalize to oldest -> newest. */
    void loadThreadTail(BackendChannel& channel,
                        const QString& rootId,
                        int perPage,
                        uint64_t lastReplyAt,
                        PageCallback callback);

    /** Seed a disconnected random middle thread window by creation timestamp. */
    void loadThreadFromTime(BackendChannel& channel,
                            const QString& rootId,
                            int perPage,
                            uint64_t fromCreateAt,
                            PageCallback callback);

    /**
     * Queue one full WebSocket post snapshot into the persistent cache and
     * return its per-backend observation sequence.
     */
    quint64 cachePostObject(const QJsonObject& postObject);

    /**
     * Invalidate a cached post after delete/reaction-only WebSocket changes and
     * return its per-backend observation sequence.
     */
    quint64 invalidateCachedPost(const QString& postId);

private:
    struct CacheAccount {
        QString server;
        QString userId;

        bool isValid() const { return !server.isEmpty() && !userId.isEmpty(); }
    };

    struct RequestContext {
        CacheAccount cacheAccount;
        quint64 observationSequence = 0;
    };

    using JsonCallback = std::function<void(QVariant,
                                            const QJsonDocument&,
                                            const RequestContext&)>;

    explicit PostRepository(Backend& backend);

    void coalescedGet(const QString& path, JsonCallback callback);

    void loadChannelCursor(BackendChannel& channel,
                           const QString& direction,
                           const QString& cursorPostId,
                           int perPage,
                           PageCallback callback,
                           bool quietIngest = false);

    void loadThread(BackendChannel& channel,
                    const QString& rootId,
                    int perPage,
                    const QString& fromPost,
                    uint64_t fromCreateAt,
                    const QString& direction,
                    PageCallback callback);

    CacheAccount currentCacheAccount() const;
    quint64 nextObservationSequence();
    void cachePosts(const CacheAccount& account,
                    const QJsonObject& postsObject,
                    quint64 observationSequence);

    static QStringList chronologicalOrder(const QJsonObject& postsObject,
                                          const QString& rootId = QString());
    static QStringList allChronologicalOrder(const QJsonObject& postsObject);
    static void ingest(BackendChannel& channel,
                       const QJsonObject& postsObject,
                       bool quiet = false);

    Backend& backend;
    HTTPConnector httpConnector;
    PostCacheService postCache;
    QHash<QString, QList<JsonCallback>> inFlightGets;
    quint64 observationSequence = 0;
};

} // namespace Mattermost
