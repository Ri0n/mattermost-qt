from pathlib import Path


def replace_once(path, old, new):
    p = Path(path)
    text = p.read_text()
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected one match, found {count}")
    p.write_text(text.replace(old, new, 1))


# ---------------------------------------------------------------------------
# Repository API: cache tail reads + authoritative provenance writes.
# ---------------------------------------------------------------------------
replace_once(
    "sources/backend/PostRepository.h",
    '''    /** Fetch an absolute main-channel page. Replies are deliberately excluded. */\n    void loadChannelPage(BackendChannel& channel,\n                         int page,\n                         int perPage,\n                         PageCallback callback);\n''',
    '''    /** Fetch an absolute main-channel page. Replies are deliberately excluded. */\n    void loadChannelPage(BackendChannel& channel,\n                         int page,\n                         int perPage,\n                         PageCallback callback);\n\n    /** Load a provenance-backed cached newest main-channel suffix. */\n    void loadCachedChannelTail(BackendChannel& channel,\n                               int limit,\n                               PageCallback callback);\n''')
replace_once(
    "sources/backend/PostRepository.h",
    '''    /** Fetch the newest replies in a thread and normalize to oldest -> newest. */\n    void loadThreadTail(BackendChannel& channel,\n                        const QString& rootId,\n                        int perPage,\n                        uint64_t lastReplyAt,\n                        PageCallback callback);\n''',
    '''    /** Fetch the newest replies in a thread and normalize to oldest -> newest. */\n    void loadThreadTail(BackendChannel& channel,\n                        const QString& rootId,\n                        int perPage,\n                        uint64_t lastReplyAt,\n                        PageCallback callback);\n\n    /** Load a provenance-backed cached newest thread-reply suffix. */\n    void loadCachedThreadTail(BackendChannel& channel,\n                              const QString& rootId,\n                              int limit,\n                              PageCallback callback);\n''')

# Add cached channel read before network loadChannelPage.
marker = '''void PostRepository::loadChannelPage(BackendChannel& channel,\n'''
insert = r'''void PostRepository::loadCachedChannelTail(BackendChannel& channel,
                                               int limit,
                                               PageCallback callback)
{
    Page miss;
    const int safeLimit = std::max(1, limit);
    const CacheAccount account = currentCacheAccount();
    if (!account.isValid() || channel.id.isEmpty()) {
        if (callback) {
            callback(miss);
        }
        return;
    }

    const quint64 readObservation = nextObservationSequence();
    QPointer<PostRepository> guard(this);
    QPointer<BackendChannel> channelGuard(&channel);
    const QString channelId = channel.id;
    postCache.loadChannelTailWindow(
        account.server, account.userId, channelId, safeLimit,
        [guard, channelGuard, account, channelId, readObservation,
         callback = std::move(callback)](QJsonObject posts) mutable {
            Page result;
            if (!guard || !channelGuard || posts.isEmpty()) {
                if (callback) {
                    callback(result);
                }
                return;
            }

            const CacheAccount current = guard->currentCacheAccount();
            if (!current.isValid() || current.server != account.server
                || current.userId != account.userId || channelGuard->id != channelId) {
                if (callback) {
                    callback(result);
                }
                return;
            }

            guard->ingestCached(*channelGuard, posts, readObservation, true);
            const QStringList ordered = chronologicalOrder(posts);
            for (const QString& id : ordered) {
                BackendPost* post = channelGuard->postIdToPost.value(id, nullptr);
                if (post && post->channel_id == channelId && post->root_id.isEmpty()
                    && !post->hidden) {
                    result.postIds.push_back(id);
                }
            }
            result.success = !result.postIds.isEmpty();
            if (callback) {
                callback(result);
            }
        });
}

'''
replace_once("sources/backend/PostRepository.cpp", marker, insert + marker)

# Record channel page-zero/10 provenance. Capture the normalized request geometry.
replace_once(
    "sources/backend/PostRepository.cpp",
    '''    coalescedGet(path,\n        [guard, channelGuard, callback = std::move(callback)](\n            QVariant status, const QJsonDocument& doc,\n            const RequestContext& requestContext) mutable {\n''',
    '''    coalescedGet(path,\n        [guard, channelGuard, safePage, safePerPage, callback = std::move(callback)](\n            QVariant status, const QJsonDocument& doc,\n            const RequestContext& requestContext) mutable {\n''')
replace_once(
    "sources/backend/PostRepository.cpp",
    '''            guard->ingest(*channelGuard, posts, requestContext.observationSequence);\n            result.postIds = chronologicalOrder(posts);\n            result.prevPostId = root.value(QStringLiteral("prev_post_id")).toString();\n''',
    '''            guard->ingest(*channelGuard, posts, requestContext.observationSequence);\n            result.postIds = chronologicalOrder(posts);\n            if (safePage == 0 && safePerPage == 10\n                && requestContext.cacheAccount.isValid()) {\n                guard->postCache.storeChannelTailWindow(\n                    requestContext.cacheAccount.server,\n                    requestContext.cacheAccount.userId,\n                    channelGuard->id, result.postIds);\n            }\n            result.prevPostId = root.value(QStringLiteral("prev_post_id")).toString();\n''')

# Add cached thread read before loadThreadTail.
marker = '''void PostRepository::loadThreadTail(BackendChannel& channel,\n'''
insert = r'''void PostRepository::loadCachedThreadTail(BackendChannel& channel,
                                              const QString& rootId,
                                              int limit,
                                              PageCallback callback)
{
    Page miss;
    const int safeLimit = std::max(1, limit);
    const CacheAccount account = currentCacheAccount();
    if (!account.isValid() || channel.id.isEmpty() || rootId.isEmpty()) {
        if (callback) {
            callback(miss);
        }
        return;
    }

    const quint64 readObservation = nextObservationSequence();
    QPointer<PostRepository> guard(this);
    QPointer<BackendChannel> channelGuard(&channel);
    const QString channelId = channel.id;
    postCache.loadThreadTailWindow(
        account.server, account.userId, channelId, rootId, safeLimit,
        [guard, channelGuard, account, channelId, rootId, readObservation,
         callback = std::move(callback)](QJsonObject posts) mutable {
            Page result;
            if (!guard || !channelGuard || posts.isEmpty()) {
                if (callback) {
                    callback(result);
                }
                return;
            }

            const CacheAccount current = guard->currentCacheAccount();
            if (!current.isValid() || current.server != account.server
                || current.userId != account.userId || channelGuard->id != channelId) {
                if (callback) {
                    callback(result);
                }
                return;
            }

            guard->ingestCached(*channelGuard, posts, readObservation, true);
            const QStringList ordered = chronologicalOrder(posts, rootId);
            for (const QString& id : ordered) {
                BackendPost* post = channelGuard->postIdToPost.value(id, nullptr);
                if (post && post->channel_id == channelId && post->root_id == rootId) {
                    result.postIds.push_back(id);
                }
            }
            result.success = !result.postIds.isEmpty();
            if (callback) {
                callback(result);
            }
        });
}

'''
replace_once("sources/backend/PostRepository.cpp", marker, insert + marker)

# In generic thread load, remember which responses actually prove the newest edge.
replace_once(
    "sources/backend/PostRepository.cpp",
    '''    const bool initialPage = safeDirection == QLatin1String("down")\n        && fromPost.isEmpty() && effectiveFromCreateAt == 0;\n''',
    '''    const bool initialPage = safeDirection == QLatin1String("down")\n        && fromPost.isEmpty() && effectiveFromCreateAt == 0;\n    const bool tailPage = safeDirection == QLatin1String("up") && fromPost.isEmpty();\n''')
replace_once(
    "sources/backend/PostRepository.cpp",
    '''    coalescedGet(path,\n        [guard, channelGuard, rootId, fromPost, initialPage,\n         callback = std::move(callback)](QVariant status, const QJsonDocument& doc,\n''',
    '''    coalescedGet(path,\n        [guard, channelGuard, rootId, fromPost, initialPage, tailPage,\n         callback = std::move(callback)](QVariant status, const QJsonDocument& doc,\n''')
replace_once(
    "sources/backend/PostRepository.cpp",
    '''            result.prevPostId = root.value(QStringLiteral("prev_post_id")).toString();\n            result.nextPostId = root.value(QStringLiteral("next_post_id")).toString();\n            result.hasNext = root.value(QStringLiteral("has_next")).toBool();\n            if (callback) {\n''',
    '''            result.prevPostId = root.value(QStringLiteral("prev_post_id")).toString();\n            result.nextPostId = root.value(QStringLiteral("next_post_id")).toString();\n            result.hasNext = root.value(QStringLiteral("has_next")).toBool();\n\n            const bool initialReachedNewest = initialPage && !result.hasNext\n                && result.nextPostId.isEmpty();\n            if ((tailPage || initialReachedNewest)\n                && requestContext.cacheAccount.isValid()) {\n                QStringList tailIds = chronologicalOrder(posts, rootId);\n                tailIds.removeAll(rootId);\n                guard->postCache.storeThreadTailWindow(\n                    requestContext.cacheAccount.server,\n                    requestContext.cacheAccount.userId,\n                    channelGuard->id, rootId, tailIds);\n            }\n            if (callback) {\n''')

# ---------------------------------------------------------------------------
# Channel source provisional newest hydration.
# ---------------------------------------------------------------------------
replace_once(
    "sources/chat-area/ChannelPostSource.h",
    '''    void seedCachedPosts();\n    void seedUnknownNewestPost();\n''',
    '''    void seedCachedPosts();\n    void seedUnknownNewestPost();\n    void hydrateCachedTail();\n    void validateCachedTail();\n''')
replace_once(
    "sources/chat-area/ChannelPostSource.cpp",
    '''    connect(&channel, &BackendChannel::onNewPosts, this,\n            [this](const ChannelNewPosts&) {\n        if (!hasRootCountEstimate) {\n            return;\n        }\n        resizeLogicalTail(currentLogicalCount());\n    });\n}\n''',
    '''    connect(&channel, &BackendChannel::onNewPosts, this,\n            [this](const ChannelNewPosts&) {\n        if (!hasRootCountEstimate) {\n            return;\n        }\n        resizeLogicalTail(currentLogicalCount());\n    });\n\n    if (hasRootCountEstimate) {\n        QTimer::singleShot(0, this, [this] { hydrateCachedTail(); });\n    }\n}\n''')

marker = '''void ChannelPostSource::removeLogicalRange(int first, int count)\n'''
insert = r'''void ChannelPostSource::hydrateCachedTail()
{
    if (!hasRootCountEstimate || postIds.isEmpty()) {
        return;
    }

    PostTimelineService& repository = PostTimelineService::instance(backend);
    repository.recordChannelOpened(channel.id);
    QPointer<ChannelPostSource> guard(this);
    repository.loadCachedChannelTail(
        channel, ServerPageSize,
        [guard](const PostTimelineService::Page& result) {
            if (!guard || !result.success || result.postIds.isEmpty()
                || guard->provisionalWindow.isValid()) {
                return;
            }

            BackendPost* newest = guard->channel.postIdToPost.value(
                result.postIds.last(), nullptr);
            if (!newest || (guard->channel.last_post_at != 0
                            && newest->create_at != guard->channel.last_post_at)) {
                qCDebug(lcTimelineChannel).nospace()
                    << "CACHE_TAIL_SKIP channel=" << guard->channel.id
                    << " reason=newest-mismatch cached="
                    << (newest ? newest->create_at : 0)
                    << " channel=" << guard->channel.last_post_at;
                return;
            }

            const int usableCount = std::min(
                static_cast<int>(result.postIds.size()),
                static_cast<int>(guard->postIds.size()));
            if (usableCount <= 0) {
                return;
            }
            const QStringList ids = result.postIds.mid(
                result.postIds.size() - usableCount);
            const int first = static_cast<int>(guard->postIds.size()) - usableCount;

            // A cache window may fill empty startup slots, but it may not move
            // or overwrite an identity the source already mapped independently.
            for (int offset = 0; offset < usableCount; ++offset) {
                const int target = first + offset;
                const QString& id = ids.at(offset);
                const int existingIndex = guard->indexOfPost(id);
                if ((existingIndex >= 0 && existingIndex != target)
                    || (!guard->postIds.at(target).isEmpty()
                        && guard->postIds.at(target) != id)) {
                    qCDebug(lcTimelineChannel).nospace()
                        << "CACHE_TAIL_SKIP channel=" << guard->channel.id
                        << " reason=identity-collision target=" << target
                        << " id=" << id;
                    return;
                }
            }

            for (const QString& id : ids) {
                if (guard->indexOfPost(id) < 0) {
                    guard->provisionalPostIds.insert(id);
                }
            }
            const ExactWindowMutation mutation = guard->assignExactWindow(first, ids);
            guard->publishExactWindow(mutation);
            qCDebug(lcTimelineChannel).nospace()
                << "CACHE_TAIL_HYDRATE channel=" << guard->channel.id
                << " first=" << first
                << " last=" << (first + usableCount - 1)
                << " count=" << usableCount;
            guard->validateCachedTail();
        });
}

void ChannelPostSource::validateCachedTail()
{
    QPointer<ChannelPostSource> guard(this);
    PostTimelineService::instance(backend).loadChannelPage(
        channel, 0, ServerPageSize,
        [guard](const PostTimelineService::Page& result) {
            if (!guard || !result.success) {
                return;
            }
            if (result.postIds.isEmpty()) {
                guard->reconcileRootCount(0);
                return;
            }
            guard->placePage(0, result.postIds);
        });
}

'''
replace_once("sources/chat-area/ChannelPostSource.cpp", marker, insert + marker)

# ---------------------------------------------------------------------------
# Thread source provisional newest hydration.
# ---------------------------------------------------------------------------
replace_once(
    "sources/chat-area/ThreadPostSource.h",
    '#include <QString>\n',
    '#include <QSet>\n#include <QString>\n')
replace_once(
    "sources/chat-area/ThreadPostSource.h",
    '''    void seedCachedPosts();\n    void placeExactWindow(int first, const QStringList& ids);\n''',
    '''    void seedCachedPosts();\n    void hydrateCachedTail();\n    void validateCachedTail();\n    bool isAuthoritativeIndex(int index) const;\n    void pruneProvisionalPostIds();\n    void placeExactWindow(int first, const QStringList& ids);\n''')
replace_once(
    "sources/chat-area/ThreadPostSource.h",
    '''    Backend& backend;\n    QString rootId;\n''',
    '''    Backend& backend;\n    QString rootId;\n    QSet<QString> provisionalPostIds;\n''')
replace_once(
    "sources/chat-area/ThreadPostSource.cpp",
    '''    connect(&channel, &BackendChannel::onPostDeleted, this,\n            [this](const QString& postId) {\n        const int index = indexOfPost(postId);\n        if (index >= 0) {\n            emit itemsChanged(index, index);\n        }\n    });\n}\n''',
    '''    connect(&channel, &BackendChannel::onPostDeleted, this,\n            [this](const QString& postId) {\n        const int index = indexOfPost(postId);\n        if (index >= 0) {\n            emit itemsChanged(index, index);\n        }\n    });\n\n    QTimer::singleShot(0, this, [this] { hydrateCachedTail(); });\n}\n''')
# Need QTimer include.
replace_once(
    "sources/chat-area/ThreadPostSource.cpp",
    '#include <QPointer>\n',
    '#include <QPointer>\n#include <QTimer>\n')

# Treat provisional identities as missing for retrieval and never as cursor anchors.
replace_once(
    "sources/chat-area/ThreadPostSource.cpp",
    '''    for (int index = requestedFirst; index <= requestedLast; ++index) {\n        if (!postIds.at(index).isEmpty()) {\n            continue;\n        }\n''',
    '''    for (int index = requestedFirst; index <= requestedLast; ++index) {\n        if (isAuthoritativeIndex(index)) {\n            continue;\n        }\n''')
replace_once(
    "sources/chat-area/ThreadPostSource.cpp",
    '''    if (firstMissing > 0 && !postIds.at(firstMissing - 1).isEmpty()) {\n''',
    '''    if (firstMissing > 0 && isAuthoritativeIndex(firstMissing - 1)) {\n''')
replace_once(
    "sources/chat-area/ThreadPostSource.cpp",
    '''        && lastMissing + 1 < postIds.size()\n        && !postIds.at(lastMissing + 1).isEmpty()) {\n''',
    '''        && lastMissing + 1 < postIds.size()\n        && isAuthoritativeIndex(lastMissing + 1)) {\n''')

marker = '''void ThreadPostSource::placeExactWindow(int first, const QStringList& ids)\n'''
insert = r'''void ThreadPostSource::hydrateCachedTail()
{
    BackendPost* root = rootPost();
    if (!root || postIds.size() <= 1 || root->reply_count <= 0) {
        return;
    }

    PostTimelineService& repository = PostTimelineService::instance(backend);
    repository.recordChannelOpened(channel.id);
    QPointer<ThreadPostSource> guard(this);
    repository.loadCachedThreadTail(
        channel, rootId, ServerBlockSize,
        [guard](const PostTimelineService::Page& result) {
            if (!guard || !result.success || result.postIds.isEmpty()) {
                return;
            }
            BackendPost* root = guard->rootPost();
            BackendPost* newest = guard->channel.postIdToPost.value(
                result.postIds.last(), nullptr);
            if (!root || !newest || (root->last_reply_at != 0
                                     && newest->create_at != root->last_reply_at)) {
                qCDebug(lcThreadTimelineTrace).nospace()
                    << "THREAD_CACHE_TAIL_SKIP source="
                    << static_cast<const void*>(guard.data())
                    << " reason=newest-mismatch cached="
                    << (newest ? newest->create_at : 0)
                    << " root=" << (root ? root->last_reply_at : 0);
                return;
            }

            const int usableCount = std::min(
                static_cast<int>(result.postIds.size()),
                static_cast<int>(guard->postIds.size()) - 1);
            if (usableCount <= 0) {
                return;
            }
            const QStringList ids = result.postIds.mid(
                result.postIds.size() - usableCount);
            const int first = static_cast<int>(guard->postIds.size()) - usableCount;
            for (int offset = 0; offset < usableCount; ++offset) {
                const int target = first + offset;
                const QString& id = ids.at(offset);
                const int existingIndex = guard->indexOfPost(id);
                if ((existingIndex >= 0 && existingIndex != target)
                    || (!guard->postIds.at(target).isEmpty()
                        && guard->postIds.at(target) != id)) {
                    qCDebug(lcThreadTimelineTrace).nospace()
                        << "THREAD_CACHE_TAIL_SKIP source="
                        << static_cast<const void*>(guard.data())
                        << " reason=identity-collision target=" << target
                        << " id=" << shortId(id);
                    return;
                }
            }

            for (const QString& id : ids) {
                if (guard->indexOfPost(id) < 0) {
                    guard->provisionalPostIds.insert(id);
                }
            }
            const ExactWindowMutation mutation = guard->assignExactWindow(first, ids);
            guard->publishExactWindow(mutation);
            qCDebug(lcThreadTimelineTrace).nospace()
                << "THREAD_CACHE_TAIL_HYDRATE source="
                << static_cast<const void*>(guard.data())
                << " target=[" << first << ',' << (first + usableCount - 1) << ']'
                << " ids=" << idsSummary(ids);
            guard->validateCachedTail();
        });
}

void ThreadPostSource::validateCachedTail()
{
    BackendPost* root = rootPost();
    if (!root) {
        return;
    }

    QPointer<ThreadPostSource> guard(this);
    if (postIds.size() - 1 <= ServerBlockSize) {
        PostTimelineService::instance(backend).loadThreadPage(
            channel, rootId, ServerBlockSize, QString(), 0,
            [guard](const PostTimelineService::Page& result) {
                if (!guard || !result.success || result.postIds.isEmpty()) {
                    return;
                }
                guard->placeInitial(result.postIds);
            });
        return;
    }

    PostTimelineService::instance(backend).loadThreadTail(
        channel, rootId, ServerBlockSize, root->last_reply_at,
        [guard](const PostTimelineService::Page& result) {
            if (!guard || !result.success || result.postIds.isEmpty()) {
                return;
            }
            guard->placeTail(result.postIds);
        });
}

bool ThreadPostSource::isAuthoritativeIndex(int index) const
{
    if (index < 0 || index >= postIds.size()) {
        return false;
    }
    const QString& id = postIds.at(index);
    return !id.isEmpty() && !provisionalPostIds.contains(id);
}

void ThreadPostSource::pruneProvisionalPostIds()
{
    for (auto it = provisionalPostIds.begin(); it != provisionalPostIds.end();) {
        if (indexOfPost(*it) < 0) {
            it = provisionalPostIds.erase(it);
        } else {
            ++it;
        }
    }
}

'''
replace_once("sources/chat-area/ThreadPostSource.cpp", marker, insert + marker)
replace_once(
    "sources/chat-area/ThreadPostSource.cpp",
    '''void ThreadPostSource::placeExactWindow(int first, const QStringList& ids)\n{\n    publishExactWindow(assignExactWindow(first, ids));\n}\n''',
    '''void ThreadPostSource::placeExactWindow(int first, const QStringList& ids)\n{\n    for (const QString& id : ids) {\n        provisionalPostIds.remove(id);\n    }\n    publishExactWindow(assignExactWindow(first, ids));\n    pruneProvisionalPostIds();\n}\n''')

# ---------------------------------------------------------------------------
# Documentation: row bags vs provenance and enabled hydration.
# ---------------------------------------------------------------------------
replace_once(
    "docs/post-cache-runtime.md",
    '''Only fields required for lookup, ordering and eviction are duplicated into SQLite columns:\n\n```text\npost_id\nchannel_id\nroot_id\ncreate_at\nupdate_at\nlast_access\ncompressed raw JSON payload\n```\n''',
    '''Only fields required for lookup, ordering and eviction are duplicated into SQLite post columns:\n\n```text\npost_id\nchannel_id\nroot_id\ncreate_at\nupdate_at\nlast_access\ncompressed raw JSON payload\n```\n\nA separate `tail_windows` table records the ordered IDs of server responses that actually proved a\nnewest edge. This provenance is essential: a set of individually cached rows is not a contiguous\nwindow. Direct post lookups, reaction invalidation and LRU eviction can all create holes. Window reads\ntherefore ignore arbitrary row bags and return only the newest still-complete suffix after the last\nmissing/corrupt row.\n''')
replace_once(
    "docs/post-cache-runtime.md",
    '''## Newest-window hydration contract\n\nThe next read-side step is bounded newest-window hydration for channels and threads. The design is\nalready constrained even before the final wiring is enabled.\n''',
    '''## Newest-window hydration contract\n\nBounded newest-window hydration is enabled for channels with a logical count estimate and for threads.\nIt consumes only provenance-backed tail windows; arbitrary cached rows never become a range.\n''')
replace_once(
    "docs/post-cache-runtime.md",
    '''A cached set of recent root posts may give an immediate first paint, but SQLite does not know the\ncurrent absolute `/posts?page=N&per_page=10` grid after remote traffic changed the channel.\n''',
    '''A cached contiguous tail window may give an immediate first paint, but SQLite does not know the\ncurrent absolute `/posts?page=N&per_page=10` grid after remote traffic changed the channel. The source\nalso requires the cached newest post timestamp to match current channel `last_post_at`; otherwise the\nwindow is retained only as ordinary cached bodies and is not mapped as the current suffix.\n''')
replace_once(
    "docs/post-cache-runtime.md",
    '''A cached thread read contains the root plus a bounded set of recent replies. The same rule applies:\ncache data can provide provisional newest reply identities, but the thread source still needs initial,\ntail or cursor evidence before claiming exact logical adjacency when the full thread is not cached.\n''',
    '''A provenance-backed thread window contains a bounded ordered set of recent replies. The source\nrequires its newest cached reply timestamp to match the root's current `last_reply_at`, then publishes\nthose IDs provisionally into tail slots. Provisional IDs are displayable but are deliberately treated\nas missing for request planning and may not serve as thread cursors. A normal initial/tail HTTP request\nis dispatched immediately and upgrades/replaces the mapping with endpoint-authoritative adjacency.\n''')

replace_once(
    "docs/post-cache.md",
    '''Read-side status:\n\n- direct `loadPost()` cache hit followed by background HTTP validation is implemented;\n- still required: seed a small newest channel/thread window from SQLite before normal server range fetch.\n''',
    '''Read-side status:\n\n- direct `loadPost()` cache hit followed by background HTTP validation is implemented;\n- provenance-backed newest channel/thread window hydration is implemented as provisional first paint\n  followed immediately by normal authoritative HTTP validation;\n- arbitrary cached row bags are never packed into a logical range.\n''')

print("provisional cache hydration patch applied")
