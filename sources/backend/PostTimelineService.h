/**
 * @file PostTimelineService.h
 * @brief Windowed Mattermost post retrieval for channel and thread timelines.
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

namespace Mattermost {

class Backend;
class BackendChannel;

class PostTimelineService : public QObject
{
    Q_OBJECT
public:
    struct Page {
        QStringList postIds; // chronological: oldest -> newest
        QString prevPostId;
        QString nextPostId;
        QString firstUnreadPostId;
        bool hasNext = false;
        bool success = false;
    };

    using PageCallback = std::function<void(const Page&)>;

    static PostTimelineService& instance(Backend& backend);

    /** Fetch an absolute main-channel page. Replies are deliberately excluded. */
    void loadChannelPage(BackendChannel& channel,
                         int page,
                         int perPage,
                         PageCallback callback);

    /**
     * Fetch the server-selected initial window around the user's unread
     * boundary, matching Mattermost webapp's first-channel-open path.
     */
    void loadChannelUnread(BackendChannel& channel,
                           int limitBefore,
                           int limitAfter,
                           uint64_t lastViewedAt,
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

    /**
     * Fetch replies after a thread cursor. Empty cursor starts at the root.
     * Mattermost always includes the root in paginated thread responses; the
     * returned Page contains it only for the initial request.
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

    /**
     * Fetch the newest replies in a thread without walking every older page.
     * The result excludes the root and is normalized to oldest -> newest.
     */
    void loadThreadTail(BackendChannel& channel,
                        const QString& rootId,
                        int perPage,
                        uint64_t lastReplyAt,
                        PageCallback callback);

    /**
     * Approximate random seek into a large thread by creation timestamp. This
     * is used when the user drags the scrollbar into an unloaded gap.
     */
    void loadThreadFromTime(BackendChannel& channel,
                            const QString& rootId,
                            int perPage,
                            uint64_t fromCreateAt,
                            PageCallback callback);

private:
    using JsonCallback = std::function<void(QVariant, const QJsonDocument&)>;

    explicit PostTimelineService(Backend& backend);

    /**
     * One HTTP request per exact timeline URL. Concurrent callers fan out from
     * the same response instead of issuing duplicate range requests. Each
     * caller still performs its own idempotent BackendChannel ingestion/parser.
     */
    void coalescedGet(const QString& path, JsonCallback callback);

    void loadChannelCursor(BackendChannel& channel,
                           const QString& direction,
                           const QString& cursorPostId,
                           int perPage,
                           PageCallback callback);

    void loadThread(BackendChannel& channel,
                    const QString& rootId,
                    int perPage,
                    const QString& fromPost,
                    uint64_t fromCreateAt,
                    const QString& direction,
                    PageCallback callback);

    static QStringList chronologicalOrder(const QJsonObject& postsObject,
                                          const QString& rootId = QString());
    static QStringList allChronologicalOrder(const QJsonObject& postsObject);
    static void ingest(BackendChannel& channel, const QJsonObject& postsObject);

    Backend& backend;
    HTTPConnector httpConnector;
    QHash<QString, QList<JsonCallback>> inFlightGets;
};

} // namespace Mattermost
