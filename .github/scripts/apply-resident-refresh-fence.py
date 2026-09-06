from pathlib import Path


def replace_once(path, old, new):
    p = Path(path)
    text = p.read_text()
    if text.count(old) != 1:
        raise RuntimeError(f"{path}: expected one match, found {text.count(old)}")
    p.write_text(text.replace(old, new, 1))


def replace_all(path, old, new, expected):
    p = Path(path)
    text = p.read_text()
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{path}: expected {expected} matches, found {count}")
    p.write_text(text.replace(old, new))


replace_once(
    "sources/backend/types/BackendPost.h",
    "\tvoid updatePostEdits (BackendPost& editedPost);\n",
    "\t/** Replace all server-backed fields while keeping this object's address stable. */\n"
    "\tbool updatePostEdits (BackendPost& editedPost);\n"
    "\t/** Parse and apply one authoritative full post snapshot in place. */\n"
    "\tbool refreshFromJson (const QJsonObject& jsonObject, const Storage& storage);\n",
)

replace_once(
    "sources/backend/types/BackendPost.cpp",
    '''void BackendPost::updatePostEdits (BackendPost& editedPost)\n{\n\tmessage = editedPost.message;\n\n\tif (poll && editedPost.poll) {\n\n\t\teditedPost.poll->metadata = poll->metadata;\n\t\tpoll = std::move (editedPost.poll);\n\t}\n\n}\n''',
    '''bool BackendPost::refreshFromJson (const QJsonObject& jsonObject, const Storage& storage)\n{\n\t// A few fields are local annotations rather than Mattermost post fields. A\n\t// raw REST/cache snapshot normally does not contain them, so absence must not\n\t// erase information already derived by the client.\n\tQJsonObject normalized = jsonObject;\n\tif (!normalized.contains(QStringLiteral("_mmqt_sender_name")) && !sender_name.isEmpty()) {\n\t\tnormalized.insert(QStringLiteral("_mmqt_sender_name"), sender_name);\n\t}\n\tif (!normalized.contains(QStringLiteral("_mmqt_current_user_mentioned"))) {\n\t\tnormalized.insert(QStringLiteral("_mmqt_current_user_mentioned"), currentUserMentioned);\n\t}\n\n\tBackendPost refreshed(normalized, storage);\n\tif (!refreshed.root_id.isEmpty()) {\n\t\trefreshed.hidden = true;\n\t}\n\treturn updatePostEdits(refreshed);\n}\n\nbool BackendPost::updatePostEdits (BackendPost& editedPost)\n{\n\t// Identity/topology is immutable for a Mattermost post. Refusing a malformed\n\t// snapshot here is safer than moving an existing object to another timeline\n\t// while widgets and sources still hold its stable address/ID.\n\tif (editedPost.id.isEmpty() || editedPost.id != id\n\t\t|| editedPost.channel_id != channel_id\n\t\t|| editedPost.root_id != root_id\n\t\t|| editedPost.create_at != create_at) {\n\t\tLOG_DEBUG("Ignoring structurally inconsistent refresh for post " << id);\n\t\treturn false;\n\t}\n\n\tconst auto sameFiles = [](const std::list<BackendFile>& lhs,\n\t                          const std::list<BackendFile>& rhs) {\n\t\tif (lhs.size() != rhs.size()) {\n\t\t\treturn false;\n\t\t}\n\t\tauto left = lhs.cbegin();\n\t\tauto right = rhs.cbegin();\n\t\tfor (; left != lhs.cend(); ++left, ++right) {\n\t\t\tif (left->id != right->id || left->name != right->name\n\t\t\t\t|| left->mimeType != right->mimeType || left->size != right->size\n\t\t\t\t|| left->extension != right->extension) {\n\t\t\t\treturn false;\n\t\t\t}\n\t\t}\n\t\treturn true;\n\t};\n\n\tconst bool nextDeleted = editedPost.delete_at != 0 || editedPost.isDeleted;\n\tconst bool nextHidden = editedPost.hidden || !root_id.isEmpty();\n\tconst bool changed = update_at != editedPost.update_at\n\t\t|| edit_at != editedPost.edit_at\n\t\t|| delete_at != editedPost.delete_at\n\t\t|| is_pinned != editedPost.is_pinned\n\t\t|| user_id != editedPost.user_id\n\t\t|| sender_name != editedPost.sender_name\n\t\t|| author != editedPost.author\n\t\t|| parent_id != editedPost.parent_id\n\t\t|| original_id != editedPost.original_id\n\t\t|| message != editedPost.message\n\t\t|| type != editedPost.type\n\t\t|| props != editedPost.props\n\t\t|| hashtags != editedPost.hashtags\n\t\t|| pending_post_id != editedPost.pending_post_id\n\t\t|| !sameFiles(files, editedPost.files)\n\t\t|| reactions != editedPost.reactions\n\t\t|| reply_count != editedPost.reply_count\n\t\t|| last_reply_at != editedPost.last_reply_at\n\t\t|| threadParticipantUserIds != editedPost.threadParticipantUserIds\n\t\t|| currentUserMentioned != editedPost.currentUserMentioned\n\t\t|| has_thread != editedPost.has_thread\n\t\t|| isDeleted != nextDeleted\n\t\t|| hidden != nextHidden\n\t\t|| static_cast<bool>(poll) != static_cast<bool>(editedPost.poll);\n\n\tif (!changed) {\n\t\treturn false;\n\t}\n\n\t// Poll vote/admin metadata is fetched through a separate endpoint. Preserve\n\t// that local enrichment while rebuilding the post-backed poll definition.\n\tif (poll && editedPost.poll) {\n\t\teditedPost.poll->metadata = poll->metadata;\n\t}\n\n\tupdate_at = editedPost.update_at;\n\tedit_at = editedPost.edit_at;\n\tdelete_at = editedPost.delete_at;\n\tis_pinned = editedPost.is_pinned;\n\tuser_id = editedPost.user_id;\n\tsender_name = editedPost.sender_name;\n\tauthor = editedPost.author;\n\tparent_id = editedPost.parent_id;\n\toriginal_id = editedPost.original_id;\n\tmessage = editedPost.message;\n\ttype = editedPost.type;\n\tprops = editedPost.props;\n\thashtags = editedPost.hashtags;\n\tpending_post_id = editedPost.pending_post_id;\n\tfiles = std::move(editedPost.files);\n\treactions = std::move(editedPost.reactions);\n\treply_count = editedPost.reply_count;\n\tlast_reply_at = editedPost.last_reply_at;\n\tthreadParticipantUserIds = std::move(editedPost.threadParticipantUserIds);\n\tpoll = std::move(editedPost.poll);\n\tcurrentUserMentioned = editedPost.currentUserMentioned;\n\thas_thread = editedPost.has_thread;\n\tisDeleted = nextDeleted;\n\thidden = nextHidden;\n\treturn true;\n}\n''',
)

replace_once(
    "sources/backend/types/BackendChannel.cpp",
    '''\tfor (int orderIndex = orderArray.size() - 1; orderIndex >= 0; --orderIndex) {\n\t\tconst QString newPostId = orderArray.at(orderIndex).toString();\n\t\tif (newPostId.isEmpty() || postIdToPost.contains(newPostId)) {\n\t\t\tcontinue;\n\t\t}\n\n\t\tconst auto postIt = postsObject.constFind(newPostId);\n\t\tif (postIt == postsObject.constEnd() || !postIt->isObject()) {\n\t\t\tcontinue;\n\t\t}\n\n\t\tconst QJsonObject postObject = postIt->toObject();\n''',
    '''\tfor (int orderIndex = orderArray.size() - 1; orderIndex >= 0; --orderIndex) {\n\t\tconst QString newPostId = orderArray.at(orderIndex).toString();\n\t\tif (newPostId.isEmpty()) {\n\t\t\tcontinue;\n\t\t}\n\n\t\tconst auto postIt = postsObject.constFind(newPostId);\n\t\tif (postIt == postsObject.constEnd() || !postIt->isObject()) {\n\t\t\tcontinue;\n\t\t}\n\n\t\tconst QJsonObject postObject = postIt->toObject();\n\t\tconst auto existing = postIdToPost.constFind(newPostId);\n\t\tif (existing != postIdToPost.cend()) {\n\t\t\tBackendPost* const existingPost = existing.value();\n\t\t\tif (existingPost && existingPost->refreshFromJson(postObject, storage)) {\n\t\t\t\temit onPostEdited(*existingPost);\n\t\t\t}\n\t\t\tcontinue;\n\t\t}\n''',
)

replace_once(
    "sources/backend/types/BackendChannel.cpp",
    '''\texistingPost->updatePostEdits (newPost);\n\temit onPostEdited (*existingPost);\n''',
    '''\tif (existingPost->updatePostEdits(newPost)) {\n\t\temit onPostEdited(*existingPost);\n\t}\n''',
)

replace_once(
    "sources/backend/PostRepository.h",
    '''    static void ingest(BackendChannel& channel,\n                       const QJsonObject& postsObject,\n                       bool quiet = false);\n''',
    '''    void ingest(BackendChannel& channel,\n                const QJsonObject& postsObject,\n                quint64 sourceObservation,\n                bool quiet = false);\n    void noteResidentObservation(const QJsonObject& postObject, quint64 observation);\n    void noteResidentPostObservation(const QString& postId, quint64 observation);\n    void pruneResidentObservations();\n''',
)

replace_once(
    "sources/backend/PostRepository.h",
    '''    QHash<QString, QList<JsonCallback>> inFlightGets;\n    QHash<QString, qint64> channelOpenedAtByAccount;\n    quint64 observationSequence = 0;\n''',
    '''    struct ResidentObservation {\n        quint64 sequence = 0;\n        qint64 touchedAt = 0;\n    };\n\n    QHash<QString, QList<JsonCallback>> inFlightGets;\n    QHash<QString, qint64> channelOpenedAtByAccount;\n    QHash<QString, ResidentObservation> residentObservations;\n    quint64 observationSequence = 0;\n''',
)

replace_once(
    "sources/backend/PostRepository.cpp",
    '#include <QHash>\n',
    '#include <QDateTime>\n#include <QHash>\n',
)

replace_once(
    "sources/backend/PostRepository.cpp",
    'constexpr int ContextFetchPerSide = 30;\n',
    'constexpr int ContextFetchPerSide = 30;\nconstexpr int ResidentObservationPruneThreshold = 16384;\nconstexpr qint64 ResidentObservationLifetimeMs = 60LL * 60 * 1000;\n',
)

replace_once(
    "sources/backend/PostRepository.cpp",
    '''quint64 PostRepository::cachePostObject(const QJsonObject& postObject)\n{\n    const quint64 sourceObservation = nextObservationSequence();\n    const QString postId = postObject.value(QStringLiteral("id")).toString();\n    if (postId.isEmpty()) {\n        return sourceObservation;\n    }\n\n    QJsonObject posts;\n    posts.insert(postId, postObject);\n    cachePosts(currentCacheAccount(), posts, sourceObservation);\n    return sourceObservation;\n}\n\nquint64 PostRepository::invalidateCachedPost(const QString& postId)\n{\n    const quint64 sourceObservation = nextObservationSequence();\n    if (postId.isEmpty()) {\n        return sourceObservation;\n    }\n    const CacheAccount account = currentCacheAccount();\n    if (account.isValid()) {\n        postCache.removePost(account.server, account.userId, postId, sourceObservation);\n    }\n    return sourceObservation;\n}\n''',
    '''quint64 PostRepository::cachePostObject(const QJsonObject& postObject)\n{\n    const quint64 sourceObservation = nextObservationSequence();\n    const QString postId = postObject.value(QStringLiteral("id")).toString();\n    if (postId.isEmpty()) {\n        return sourceObservation;\n    }\n\n    // This observation is relevant to resident causality even when the channel\n    // is outside the disk-admission horizon. A previously dispatched HTTP\n    // request must not overwrite a newer WebSocket snapshot in memory. Replies\n    // also advance root thread metadata, so fence their root ID conservatively.\n    noteResidentObservation(postObject, sourceObservation);\n\n    const QString channelId = postObject.value(QStringLiteral("channel_id")).toString();\n    if (shouldCacheChannelOnDisk(channelId)) {\n        QJsonObject posts;\n        posts.insert(postId, postObject);\n        cachePosts(currentCacheAccount(), posts, sourceObservation);\n    }\n    pruneResidentObservations();\n    return sourceObservation;\n}\n\nquint64 PostRepository::invalidateCachedPost(const QString& postId)\n{\n    const quint64 sourceObservation = nextObservationSequence();\n    if (postId.isEmpty()) {\n        return sourceObservation;\n    }\n\n    // Delete/reaction events may race an already in-flight REST response. Keep\n    // a short-lived resident watermark even if the post is not currently\n    // materialized, otherwise that stale response could resurrect it.\n    noteResidentPostObservation(postId, sourceObservation);\n\n    const CacheAccount account = currentCacheAccount();\n    if (account.isValid()) {\n        postCache.removePost(account.server, account.userId, postId, sourceObservation);\n    }\n    pruneResidentObservations();\n    return sourceObservation;\n}\n''',
)

replace_once(
    "sources/backend/PostRepository.cpp",
    '                ingest(*channel, posts, true);\n',
    '                guard->ingest(*channel, posts, requestContext.observationSequence, true);\n',
)
replace_all(
    "sources/backend/PostRepository.cpp",
    '            ingest(*channelGuard, posts);\n',
    '            guard->ingest(*channelGuard, posts, requestContext.observationSequence);\n',
    3,
)
replace_once(
    "sources/backend/PostRepository.cpp",
    '            ingest(*channelGuard, posts, quietIngest);\n',
    '            guard->ingest(*channelGuard, posts, requestContext.observationSequence, quietIngest);\n',
)

replace_once(
    "sources/backend/PostRepository.cpp",
    '''void PostRepository::ingest(BackendChannel& channel,\n                            const QJsonObject& postsObject,\n                            bool quiet)\n{\n    const QStringList chronological = allChronologicalOrder(postsObject);\n    QJsonArray newestFirst;\n    for (int i = chronological.size() - 1; i >= 0; --i) {\n        newestFirst.push_back(chronological.at(i));\n    }\n\n    if (quiet) {\n        const QSignalBlocker blocker(&channel);\n        channel.mergePostContext(newestFirst, postsObject);\n    } else {\n        channel.mergePostContext(newestFirst, postsObject);\n    }\n}\n''',
    '''void PostRepository::ingest(BackendChannel& channel,\n                            const QJsonObject& postsObject,\n                            quint64 sourceObservation,\n                            bool quiet)\n{\n    QJsonObject acceptedPosts;\n    for (auto it = postsObject.constBegin(); it != postsObject.constEnd(); ++it) {\n        if (!it->isObject()) {\n            continue;\n        }\n        const QJsonObject postObject = it->toObject();\n        const QString postId = postObject.value(QStringLiteral("id")).toString(it.key());\n        if (postId.isEmpty()) {\n            continue;\n        }\n\n        const auto watermark = residentObservations.constFind(postId);\n        if (sourceObservation != 0 && watermark != residentObservations.cend()\n            && watermark->sequence > sourceObservation) {\n            // A WebSocket/newer REST observation happened after this physical\n            // request was dispatched. Its older payload may still warm SQLite\n            // through the cache worker's independent fence, but has no resident\n            // authority.\n            continue;\n        }\n        acceptedPosts.insert(postId, postObject);\n    }\n\n    if (acceptedPosts.isEmpty()) {\n        pruneResidentObservations();\n        return;\n    }\n\n    // Mark before mutation. Other callbacks from the same coalesced physical\n    // request carry the same sequence and remain admissible; older requests do\n    // not. Reply snapshots fence their roots because mergePostContext may update\n    // root thread metadata while ingesting the reply.\n    for (auto it = acceptedPosts.constBegin(); it != acceptedPosts.constEnd(); ++it) {\n        noteResidentObservation(it->toObject(), sourceObservation);\n    }\n\n    const QStringList chronological = allChronologicalOrder(acceptedPosts);\n    QJsonArray newestFirst;\n    for (int i = chronological.size() - 1; i >= 0; --i) {\n        newestFirst.push_back(chronological.at(i));\n    }\n\n    if (quiet) {\n        const QSignalBlocker blocker(&channel);\n        channel.mergePostContext(newestFirst, acceptedPosts);\n    } else {\n        channel.mergePostContext(newestFirst, acceptedPosts);\n    }\n    pruneResidentObservations();\n}\n\nvoid PostRepository::noteResidentObservation(const QJsonObject& postObject,\n                                               quint64 observation)\n{\n    if (observation == 0) {\n        return;\n    }\n    const QString postId = postObject.value(QStringLiteral("id")).toString();\n    noteResidentPostObservation(postId, observation);\n\n    const QString rootId = postObject.value(QStringLiteral("root_id")).toString();\n    if (!rootId.isEmpty()) {\n        noteResidentPostObservation(rootId, observation);\n    }\n}\n\nvoid PostRepository::noteResidentPostObservation(const QString& postId,\n                                                  quint64 observation)\n{\n    if (postId.isEmpty() || observation == 0) {\n        return;\n    }\n    ResidentObservation& current = residentObservations[postId];\n    current.sequence = std::max(current.sequence, observation);\n    current.touchedAt = QDateTime::currentMSecsSinceEpoch();\n}\n\nvoid PostRepository::pruneResidentObservations()\n{\n    if (residentObservations.size() <= ResidentObservationPruneThreshold) {\n        return;\n    }\n\n    // These watermarks only protect against older in-flight network work; they\n    // are not persistent freshness metadata. One hour is deliberately far\n    // beyond a normal HTTP request lifetime while bounding busy-session memory.\n    const qint64 cutoff = QDateTime::currentMSecsSinceEpoch()\n        - ResidentObservationLifetimeMs;\n    for (auto it = residentObservations.begin(); it != residentObservations.end();) {\n        if (it->touchedAt < cutoff) {\n            it = residentObservations.erase(it);\n        } else {\n            ++it;\n        }\n    }\n}\n''',
)

replace_all(
    "sources/backend/WebSocketEventHandler.cpp",
    '''\t// A busy joined channel is not cache interest. Persist the full event only\n\t// while the user has opened this channel inside the configured disk horizon.\n\tif (repository.shouldCacheChannelOnDisk(event.channelId)) {\n\t\trepository.cachePostObject(event.postObject);\n\t}\n''',
    '''\t// Always record the resident observation so an older in-flight HTTP\n\t// response cannot overwrite this event. PostRepository applies disk admission\n\t// independently and persists only recently opened channels.\n\trepository.cachePostObject(event.postObject);\n''',
    1,
)
replace_once(
    "sources/backend/WebSocketEventHandler.cpp",
    '''\tauto& repository = PostRepository::instance(backend);\n\tif (repository.shouldCacheChannelOnDisk(event.channelId)) {\n\t\trepository.cachePostObject(event.postObject);\n\t}\n''',
    '''\tauto& repository = PostRepository::instance(backend);\n\t// See PostEvent: resident causality is unconditional; durable admission is\n\t// decided inside PostRepository.\n\trepository.cachePostObject(event.postObject);\n''',
)

replace_once(
    "docs/post-cache.md",
    '''The same ordering concept will be applied to resident refresh before cache-first hydration is\nenabled, so an old HTTP response cannot overwrite a newer WebSocket state in memory either.\n''',
    '''The same ordering concept is also applied to resident refresh. Each physical HTTP request keeps\nits dispatch observation sequence, while WebSocket new/edit/delete/reaction observations advance the\nresident watermark immediately. `PostRepository` drops an older response per post before it reaches\n`BackendChannel`, so stale REST work cannot overwrite a newer WebSocket state in memory. Reply\nobservations conservatively fence their root ID too because ingesting a reply can update transient\nthread metadata on the root. These watermarks are short-lived in-flight causality guards, not\npersistent cache freshness metadata.\n''',
)

replace_once(
    "docs/post-cache.md",
    '''Cache-first hydration is deliberately not enabled yet. `BackendChannel::mergePostContext()` currently\nskips a post ID that is already resident, and `BackendPost::updatePostEdits()` does not perform a full\nraw-JSON refresh. Hydrating an older SQLite snapshot first would therefore allow a later fresh HTTP\nsnapshot with the same ID to be ignored. Full existing-post refresh semantics are a prerequisite for\nusing SQLite as a read tier.\n''',
    '''Cache-first hydration is deliberately not enabled yet, but the resident refresh prerequisite is\nnow in place. `BackendChannel::mergePostContext()` refreshes an already-resident ID in place from the\naccepted full JSON snapshot, preserving the stable `BackendPost*` address used by current widgets and\nsources. `BackendPost` replaces all server-backed post fields rather than only the message, while\npreserving separately fetched poll metadata and local annotations absent from raw REST/cache JSON.\nThe next read-side step is to serve direct/newest-window cache hits as provisional resident data and\nvalidate them with newer HTTP observations before granting any absolute-page authority.\n''',
)

replace_once(
    "docs/post-cache.md",
    '''Still required before enabling reads:\n\n- full refresh semantics when fresh JSON arrives for an already resident `BackendPost`;\n- resident causal fencing against stale HTTP responses;\n- direct `loadPost()` cache hit followed by background validation;\n- seed a small newest channel/thread window from SQLite before normal server range fetch.\n''',
    '''Implemented prerequisites for reads:\n\n- full in-place refresh semantics when fresh JSON arrives for an already resident `BackendPost`;\n- resident causal fencing against stale HTTP responses.\n\nStill required to enable reads:\n\n- direct `loadPost()` cache hit followed by background validation;\n- seed a small newest channel/thread window from SQLite before normal server range fetch.\n''',
)

print("resident refresh/fence patch applied")
