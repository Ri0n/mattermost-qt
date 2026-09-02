/**
 * @file PostTimelineService.h
 * @brief Windowed Mattermost post retrieval for channel and thread timelines.
 *
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#pragma once

#include <functional>

#include <QObject>
#include <QPointer>
#include <QStringList>

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
    explicit PostTimelineService(Backend& backend);

    void loadThread(BackendChannel& channel,
                    const QString& rootId,
                    int perPage,
                    const QString& fromPost,
                    uint64_t fromCreateAt,
                    PageCallback callback);

    static QStringList chronologicalOrder(const QJsonObject& postsObject,
                                          const QString& rootId = QString());
    static void ingest(BackendChannel& channel, const QJsonObject& postsObject);

    Backend& backend;
    HTTPConnector httpConnector;
};

} // namespace Mattermost
