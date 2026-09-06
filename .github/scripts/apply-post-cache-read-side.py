from pathlib import Path


def replace_once(path, old, new):
    p = Path(path)
    text = p.read_text()
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected one match, found {count}")
    p.write_text(text.replace(old, new, 1))


# ---------------------------------------------------------------------------
# PostCacheService: asynchronous read API, callbacks marshalled to owner thread.
# ---------------------------------------------------------------------------
replace_once(
    "sources/backend/PostCacheService.h",
    "#include <QtGlobal>\n#include <QJsonObject>\n#include <QString>\n#include <QThread>\n",
    "#include <functional>\n\n#include <QtGlobal>\n#include <QJsonObject>\n#include <QObject>\n#include <QString>\n#include <QThread>\n",
)
replace_once(
    "sources/backend/PostCacheService.h",
    "class PostCacheService final\n{\npublic:\n",
    "class PostCacheService final\n{\npublic:\n    using ReadCallback = std::function<void(QJsonObject)>;\n\n",
)
replace_once(
    "sources/backend/PostCacheService.h",
    '''    /** Queue invalidation of one raw post snapshot. */\n    void removePost(const QString& server,\n                    const QString& userId,\n                    const QString& postId,\n                    quint64 observationSequence);\n\nprivate:\n    QThread workerThread;\n    PostCacheWorker* worker = nullptr;\n''',
    '''    /** Queue invalidation of one raw post snapshot. */\n    void removePost(const QString& server,\n                    const QString& userId,\n                    const QString& postId,\n                    quint64 observationSequence);\n\n    /** Read one cached post asynchronously; empty object means miss/error. */\n    void loadPost(const QString& server,\n                  const QString& userId,\n                  const QString& postId,\n                  ReadCallback callback);\n\n    /** Read newest cached channel roots asynchronously. */\n    void loadLatestChannelRoots(const QString& server,\n                                const QString& userId,\n                                const QString& channelId,\n                                int limit,\n                                ReadCallback callback);\n\n    /** Read a cached thread root plus newest replies asynchronously. */\n    void loadThread(const QString& server,\n                    const QString& userId,\n                    const QString& channelId,\n                    const QString& rootId,\n                    int limit,\n                    ReadCallback callback);\n\nprivate:\n    QObject callbackContext;\n    QThread workerThread;\n    PostCacheWorker* worker = nullptr;\n''',
)

replace_once(
    "sources/backend/PostCacheService.cpp",
    "#include <QMetaObject>\n#include <QSettings>\n",
    "#include <QMetaObject>\n#include <QPointer>\n#include <QSettings>\n",
)
replace_once(
    "sources/backend/PostCacheService.cpp",
    '''    void removePost(const QString& server,\n                    const QString& userId,\n                    const QString& postId,\n                    quint64 observationSequence)\n    {\n        if (postId.isEmpty()) {\n            return;\n        }\n\n        const QString key = watermarkKey(server, userId, postId);\n        InvalidationWatermark& watermark = invalidationWatermarks[key];\n        watermark.observationSequence = std::max(watermark.observationSequence,\n                                                 observationSequence);\n        watermark.createdAt = QDateTime::currentMSecsSinceEpoch();\n\n        if (selectAccount(server, userId)) {\n            store->removePost(postId);\n        }\n        pruneInvalidationWatermarks();\n    }\n\n    void shutdown()\n''',
    '''    void removePost(const QString& server,\n                    const QString& userId,\n                    const QString& postId,\n                    quint64 observationSequence)\n    {\n        if (postId.isEmpty()) {\n            return;\n        }\n\n        const QString key = watermarkKey(server, userId, postId);\n        InvalidationWatermark& watermark = invalidationWatermarks[key];\n        watermark.observationSequence = std::max(watermark.observationSequence,\n                                                 observationSequence);\n        watermark.createdAt = QDateTime::currentMSecsSinceEpoch();\n\n        if (selectAccount(server, userId)) {\n            store->removePost(postId);\n        }\n        pruneInvalidationWatermarks();\n    }\n\n    QJsonObject loadPost(const QString& server,\n                         const QString& userId,\n                         const QString& postId)\n    {\n        if (postId.isEmpty() || !selectAccount(server, userId)) {\n            return {};\n        }\n        return store->loadPost(postId);\n    }\n\n    QJsonObject loadLatestChannelRoots(const QString& server,\n                                       const QString& userId,\n                                       const QString& channelId,\n                                       int limit)\n    {\n        if (channelId.isEmpty() || limit <= 0 || !selectAccount(server, userId)) {\n            return {};\n        }\n        return store->loadLatestChannelRoots(channelId, limit);\n    }\n\n    QJsonObject loadThread(const QString& server,\n                           const QString& userId,\n                           const QString& channelId,\n                           const QString& rootId,\n                           int limit)\n    {\n        if (channelId.isEmpty() || rootId.isEmpty() || limit <= 0\n            || !selectAccount(server, userId)) {\n            return {};\n        }\n        return store->loadThread(channelId, rootId, limit);\n    }\n\n    void shutdown()\n''',
)

replace_once(
    "sources/backend/PostCacheService.cpp",
    '''void PostCacheService::removePost(const QString& server,\n                                  const QString& userId,\n                                  const QString& postId,\n                                  quint64 observationSequence)\n{\n    if (!worker || server.trimmed().isEmpty() || userId.trimmed().isEmpty()\n        || postId.isEmpty()) {\n        return;\n    }\n\n    PostCacheWorker* const currentWorker = worker;\n    QMetaObject::invokeMethod(currentWorker,\n                              [currentWorker, server, userId, postId,\n                               observationSequence] {\n                                  currentWorker->removePost(server, userId, postId,\n                                                           observationSequence);\n                              },\n                              Qt::QueuedConnection);\n}\n\n} // namespace Mattermost\n''',
    '''void PostCacheService::removePost(const QString& server,\n                                  const QString& userId,\n                                  const QString& postId,\n                                  quint64 observationSequence)\n{\n    if (!worker || server.trimmed().isEmpty() || userId.trimmed().isEmpty()\n        || postId.isEmpty()) {\n        return;\n    }\n\n    PostCacheWorker* const currentWorker = worker;\n    QMetaObject::invokeMethod(currentWorker,\n                              [currentWorker, server, userId, postId,\n                               observationSequence] {\n                                  currentWorker->removePost(server, userId, postId,\n                                                           observationSequence);\n                              },\n                              Qt::QueuedConnection);\n}\n\nvoid PostCacheService::loadPost(const QString& server,\n                                const QString& userId,\n                                const QString& postId,\n                                ReadCallback callback)\n{\n    QPointer<QObject> context(&callbackContext);\n    if (!worker || server.trimmed().isEmpty() || userId.trimmed().isEmpty()\n        || postId.isEmpty()) {\n        if (callback) {\n            QMetaObject::invokeMethod(&callbackContext,\n                                      [callback = std::move(callback)]() mutable {\n                                          callback({});\n                                      },\n                                      Qt::QueuedConnection);\n        }\n        return;\n    }\n\n    PostCacheWorker* const currentWorker = worker;\n    QMetaObject::invokeMethod(currentWorker,\n                              [currentWorker, server, userId, postId, context,\n                               callback = std::move(callback)]() mutable {\n                                  QJsonObject result = currentWorker->loadPost(\n                                      server, userId, postId);\n                                  if (!context || !callback) {\n                                      return;\n                                  }\n                                  QMetaObject::invokeMethod(\n                                      context.data(),\n                                      [callback = std::move(callback),\n                                       result = std::move(result)]() mutable {\n                                          callback(std::move(result));\n                                      },\n                                      Qt::QueuedConnection);\n                              },\n                              Qt::QueuedConnection);\n}\n\nvoid PostCacheService::loadLatestChannelRoots(const QString& server,\n                                              const QString& userId,\n                                              const QString& channelId,\n                                              int limit,\n                                              ReadCallback callback)\n{\n    QPointer<QObject> context(&callbackContext);\n    if (!worker || server.trimmed().isEmpty() || userId.trimmed().isEmpty()\n        || channelId.isEmpty() || limit <= 0) {\n        if (callback) {\n            QMetaObject::invokeMethod(&callbackContext,\n                                      [callback = std::move(callback)]() mutable {\n                                          callback({});\n                                      },\n                                      Qt::QueuedConnection);\n        }\n        return;\n    }\n\n    PostCacheWorker* const currentWorker = worker;\n    QMetaObject::invokeMethod(\n        currentWorker,\n        [currentWorker, server, userId, channelId, limit, context,\n         callback = std::move(callback)]() mutable {\n            QJsonObject result = currentWorker->loadLatestChannelRoots(\n                server, userId, channelId, limit);\n            if (!context || !callback) {\n                return;\n            }\n            QMetaObject::invokeMethod(\n                context.data(),\n                [callback = std::move(callback), result = std::move(result)]() mutable {\n                    callback(std::move(result));\n                },\n                Qt::QueuedConnection);\n        },\n        Qt::QueuedConnection);\n}\n\nvoid PostCacheService::loadThread(const QString& server,\n                                  const QString& userId,\n                                  const QString& channelId,\n                                  const QString& rootId,\n                                  int limit,\n                                  ReadCallback callback)\n{\n    QPointer<QObject> context(&callbackContext);\n    if (!worker || server.trimmed().isEmpty() || userId.trimmed().isEmpty()\n        || channelId.isEmpty() || rootId.isEmpty() || limit <= 0) {\n        if (callback) {\n            QMetaObject::invokeMethod(&callbackContext,\n                                      [callback = std::move(callback)]() mutable {\n                                          callback({});\n                                      },\n                                      Qt::QueuedConnection);\n        }\n        return;\n    }\n\n    PostCacheWorker* const currentWorker = worker;\n    QMetaObject::invokeMethod(\n        currentWorker,\n        [currentWorker, server, userId, channelId, rootId, limit, context,\n         callback = std::move(callback)]() mutable {\n            QJsonObject result = currentWorker->loadThread(\n                server, userId, channelId, rootId, limit);\n            if (!context || !callback) {\n                return;\n            }\n            QMetaObject::invokeMethod(\n                context.data(),\n                [callback = std::move(callback), result = std::move(result)]() mutable {\n                    callback(std::move(result));\n                },\n                Qt::QueuedConnection);\n        },\n        Qt::QueuedConnection);\n}\n\n} // namespace Mattermost\n''',
)

# ---------------------------------------------------------------------------
# PostRepository: cache-first direct lookup. Cache can insert, never refresh.
# ---------------------------------------------------------------------------
replace_once(
    "sources/backend/PostRepository.cpp",
    '''struct AroundState {\n    QPointer<BackendChannel> channel;\n    QString postId;\n    PostRepository::Page before;\n    PostRepository::Page after;\n    PostRepository::ContextCallback callback;\n    int pending = 3;\n    bool failed = false;\n};\n''',
    '''struct AroundState {\n    QPointer<BackendChannel> channel;\n    QString postId;\n    PostRepository::Page before;\n    PostRepository::Page after;\n    PostRepository::ContextCallback callback;\n    int pending = 3;\n    bool failed = false;\n};\n\nstruct DirectPostState {\n    PostRepository::PostCallback callback;\n    bool cacheDone = false;\n    bool httpDone = false;\n    bool delivered = false;\n};\n''',
)

old_load = '''void PostRepository::loadPost(const QString& postId, PostCallback callback)\n{\n    if (postId.isEmpty()) {\n        if (callback) {\n            callback(PostResult {});\n        }\n        return;\n    }\n\n    QPointer<PostRepository> guard(this);\n    coalescedGet(QStringLiteral("posts/") + postId,\n        [guard, postId, callback = std::move(callback)](\n            QVariant status, const QJsonDocument& doc,\n            const RequestContext& requestContext) mutable {\n            PostResult result;\n            result.postId = postId;\n            if (!guard || status.toInt() != QNetworkReply::NoError || !doc.isObject()) {\n                if (callback) {\n                    callback(result);\n                }\n                return;\n            }\n\n            const QJsonObject postObject = doc.object();\n            result.channelId = postObject.value(QStringLiteral("channel_id")).toString();\n            result.rootId = postObject.value(QStringLiteral("root_id")).toString();\n\n            QJsonObject posts;\n            posts.insert(postId, postObject);\n            guard->cachePosts(requestContext.cacheAccount, posts,\n                              requestContext.observationSequence);\n\n            BackendChannel* channel = guard->backend.getStorage().getChannelById(result.channelId);\n            if (channel) {\n                guard->ingest(*channel, posts, requestContext.observationSequence, true);\n                result.success = channel->postIdToPost.contains(postId);\n            }\n\n            if (callback) {\n                callback(result);\n            }\n        });\n}\n'''
new_load = '''void PostRepository::loadPost(const QString& postId, PostCallback callback)\n{\n    if (postId.isEmpty()) {\n        if (callback) {\n            callback(PostResult {});\n        }\n        return;\n    }\n\n    const CacheAccount cacheAccount = currentCacheAccount();\n    const quint64 cacheReadObservation = nextObservationSequence();\n    auto state = std::make_shared<DirectPostState>();\n    state->callback = std::move(callback);\n    QPointer<PostRepository> guard(this);\n\n    const auto deliverSuccess = [state](const PostResult& result) {\n        if (!state->delivered && result.success && state->callback) {\n            state->delivered = true;\n            state->callback(result);\n        }\n    };\n    const auto deliverFailureIfDone = [state, postId] {\n        if (!state->delivered && state->cacheDone && state->httpDone) {\n            state->delivered = true;\n            if (state->callback) {\n                PostResult result;\n                result.postId = postId;\n                state->callback(result);\n            }\n        }\n    };\n\n    if (cacheAccount.isValid()) {\n        postCache.loadPost(\n            cacheAccount.server, cacheAccount.userId, postId,\n            [guard, state, postId, cacheReadObservation,\n             deliverSuccess, deliverFailureIfDone](QJsonObject postObject) mutable {\n                state->cacheDone = true;\n                if (!guard || postObject.isEmpty()) {\n                    deliverFailureIfDone();\n                    return;\n                }\n\n                PostResult result;\n                result.postId = postId;\n                result.channelId = postObject.value(QStringLiteral("channel_id")).toString();\n                result.rootId = postObject.value(QStringLiteral("root_id")).toString();\n                BackendChannel* channel = guard->backend.getStorage().getChannelById(\n                    result.channelId);\n                if (channel) {\n                    // Cached data is deliberately weaker than resident/server\n                    // data. It may fill an absent identity, but never refresh an\n                    // already resident post. A mutation observed after this read\n                    // started also vetoes the cached insertion.\n                    if (!channel->postIdToPost.contains(postId)) {\n                        const auto watermark = guard->residentObservations.constFind(postId);\n                        if (watermark == guard->residentObservations.cend()\n                            || watermark->sequence <= cacheReadObservation) {\n                            QJsonObject posts;\n                            posts.insert(postId, postObject);\n                            guard->ingestCached(*channel, posts, cacheReadObservation, true);\n                        }\n                    }\n                    result.success = channel->postIdToPost.contains(postId);\n                }\n                deliverSuccess(result);\n                deliverFailureIfDone();\n            });\n    } else {\n        state->cacheDone = true;\n    }\n\n    // HTTP validation always runs, even after a fast cache hit. It keeps normal\n    // request coalescing and carries the physical request's observation sequence.\n    coalescedGet(QStringLiteral("posts/") + postId,\n        [guard, state, postId, deliverSuccess, deliverFailureIfDone](\n            QVariant status, const QJsonDocument& doc,\n            const RequestContext& requestContext) mutable {\n            state->httpDone = true;\n            PostResult result;\n            result.postId = postId;\n            if (!guard || status.toInt() != QNetworkReply::NoError || !doc.isObject()) {\n                deliverFailureIfDone();\n                return;\n            }\n\n            const QJsonObject postObject = doc.object();\n            result.channelId = postObject.value(QStringLiteral("channel_id")).toString();\n            result.rootId = postObject.value(QStringLiteral("root_id")).toString();\n\n            QJsonObject posts;\n            posts.insert(postId, postObject);\n            guard->cachePosts(requestContext.cacheAccount, posts,\n                              requestContext.observationSequence);\n\n            BackendChannel* channel = guard->backend.getStorage().getChannelById(result.channelId);\n            if (channel) {\n                guard->ingest(*channel, posts, requestContext.observationSequence, true);\n                result.success = channel->postIdToPost.contains(postId);\n            }\n\n            deliverSuccess(result);\n            deliverFailureIfDone();\n        });\n}\n'''
replace_once("sources/backend/PostRepository.cpp", old_load, new_load)

replace_once(
    "sources/backend/PostRepository.h",
    '''    void ingest(BackendChannel& channel,\n                const QJsonObject& postsObject,\n                quint64 sourceObservation,\n                bool quiet = false);\n    void noteResidentObservation(const QJsonObject& postObject, quint64 observation);\n''',
    '''    void ingest(BackendChannel& channel,\n                const QJsonObject& postsObject,\n                quint64 sourceObservation,\n                bool quiet = false);\n    void ingestCached(BackendChannel& channel,\n                      const QJsonObject& postsObject,\n                      quint64 readObservation,\n                      bool quiet = false);\n    void noteResidentObservation(const QJsonObject& postObject, quint64 observation);\n''',
)

insert_before = '''void PostRepository::noteResidentObservation(const QJsonObject& postObject,\n                                               quint64 observation)\n'''
ingest_cached = '''void PostRepository::ingestCached(BackendChannel& channel,\n                                  const QJsonObject& postsObject,\n                                  quint64 readObservation,\n                                  bool quiet)\n{\n    QJsonObject acceptedPosts;\n    for (auto it = postsObject.constBegin(); it != postsObject.constEnd(); ++it) {\n        if (!it->isObject()) {\n            continue;\n        }\n        const QJsonObject postObject = it->toObject();\n        const QString postId = postObject.value(QStringLiteral("id")).toString(it.key());\n        if (postId.isEmpty() || channel.postIdToPost.contains(postId)) {\n            continue;\n        }\n        const auto watermark = residentObservations.constFind(postId);\n        if (watermark != residentObservations.cend()\n            && watermark->sequence > readObservation) {\n            continue;\n        }\n        acceptedPosts.insert(postId, postObject);\n    }\n\n    if (acceptedPosts.isEmpty()) {\n        return;\n    }\n\n    const QStringList chronological = allChronologicalOrder(acceptedPosts);\n    QJsonArray newestFirst;\n    for (int i = chronological.size() - 1; i >= 0; --i) {\n        newestFirst.push_back(chronological.at(i));\n    }\n    if (quiet) {\n        const QSignalBlocker blocker(&channel);\n        channel.mergePostContext(newestFirst, acceptedPosts);\n    } else {\n        channel.mergePostContext(newestFirst, acceptedPosts);\n    }\n}\n\n'''
replace_once("sources/backend/PostRepository.cpp", insert_before, ingest_cached + insert_before)

# ---------------------------------------------------------------------------
# Service-level async read test.
# ---------------------------------------------------------------------------
Path("tests/PostCacheServiceTest.cpp").write_text(r'''#include <QDateTime>
#include <QEventLoop>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include "backend/PostCacheService.h"

using namespace Mattermost;

namespace {

QJsonObject makePost(const QString& id,
                     const QString& channelId,
                     const QString& rootId,
                     qint64 createAt)
{
    QJsonObject post;
    post.insert(QStringLiteral("id"), id);
    post.insert(QStringLiteral("channel_id"), channelId);
    post.insert(QStringLiteral("root_id"), rootId);
    post.insert(QStringLiteral("create_at"), createAt);
    post.insert(QStringLiteral("update_at"), createAt);
    post.insert(QStringLiteral("message"), id);
    return post;
}

QJsonObject waitForRead(const std::function<void(PostCacheService::ReadCallback)>& start)
{
    QEventLoop loop;
    QJsonObject result;
    bool completed = false;
    start([&](QJsonObject object) {
        result = std::move(object);
        completed = true;
        loop.quit();
    });
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    loop.exec();
    if (!completed) {
        return {};
    }
    return result;
}

} // namespace

class PostCacheServiceTest : public QObject
{
    Q_OBJECT
private slots:
    void asyncReadsFollowQueuedWrites();
};

void PostCacheServiceTest::asyncReadsFollowQueuedWrites()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString server = QStringLiteral("https://example.invalid");
    const QString user = QStringLiteral("user");
    const QString channel = QStringLiteral("channel");
    const QString root = QStringLiteral("root");
    PostCacheService service(dir.filePath(QStringLiteral("posts.sqlite3")));

    service.recordChannelOpened(server, user, channel, QDateTime::currentMSecsSinceEpoch());
    QJsonObject posts;
    posts.insert(root, makePost(root, channel, QString(), 100));
    posts.insert(QStringLiteral("reply"),
                 makePost(QStringLiteral("reply"), channel, root, 200));
    service.storePosts(server, user, posts, 1);

    const QJsonObject direct = waitForRead([&](PostCacheService::ReadCallback callback) {
        service.loadPost(server, user, root, std::move(callback));
    });
    QCOMPARE(direct.value(QStringLiteral("id")).toString(), root);

    const QJsonObject roots = waitForRead([&](PostCacheService::ReadCallback callback) {
        service.loadLatestChannelRoots(server, user, channel, 10, std::move(callback));
    });
    QVERIFY(roots.contains(root));
    QVERIFY(!roots.contains(QStringLiteral("reply")));

    const QJsonObject thread = waitForRead([&](PostCacheService::ReadCallback callback) {
        service.loadThread(server, user, channel, root, 10, std::move(callback));
    });
    QVERIFY(thread.contains(root));
    QVERIFY(thread.contains(QStringLiteral("reply")));
}

QTEST_MAIN(PostCacheServiceTest)
#include "PostCacheServiceTest.moc"
''')

replace_once(
    "tests/CMakeLists.txt",
    '''add_test(NAME post-cache-store-test COMMAND post-cache-store-test)\n''',
    '''add_test(NAME post-cache-store-test COMMAND post-cache-store-test)\n\nadd_executable(post-cache-service-test\n    PostCacheServiceTest.cpp\n    "${CMAKE_SOURCE_DIR}/sources/backend/PostCacheService.cpp"\n    "${CMAKE_SOURCE_DIR}/sources/backend/PostCacheStore.cpp"\n)\n\ntarget_include_directories(post-cache-service-test\n    PRIVATE\n        "${CMAKE_SOURCE_DIR}/sources"\n)\n\ntarget_link_libraries(post-cache-service-test\n    PRIVATE\n        Qt${QT_VERSION_MAJOR}::Test\n        Qt${QT_VERSION_MAJOR}::Core\n        Qt${QT_VERSION_MAJOR}::Sql\n)\n\nadd_test(NAME post-cache-service-test COMMAND post-cache-service-test)\n''',
)

# ---------------------------------------------------------------------------
# Documentation state.
# ---------------------------------------------------------------------------
replace_once(
    "docs/post-cache.md",
    '''The next read-side step is to serve direct/newest-window cache hits as provisional resident data and\nvalidate them with newer HTTP observations before granting any absolute-page authority.\n''',
    '''Direct post lookup now has its first cache-read path. An asynchronous SQLite hit may insert an absent\nresident post and satisfy the caller immediately, but it never refreshes an already-resident object and\nnever advances the resident server-observation watermark. The normal HTTP request is still dispatched\nand validates/refreshes that object in the background; a failed result is delivered only after both\ncache and HTTP miss. Cached identity/timestamps still have no absolute-page authority. Newest channel\nand thread window hydration remains the next read-side step.\n''',
)
replace_once(
    "docs/post-cache.md",
    '''Still required to enable reads:\n\n- direct `loadPost()` cache hit followed by background validation;\n- seed a small newest channel/thread window from SQLite before normal server range fetch.\n''',
    '''Read-side status:\n\n- direct `loadPost()` cache hit followed by background HTTP validation is implemented;\n- still required: seed a small newest channel/thread window from SQLite before normal server range fetch.\n''',
)

print("post cache read-side patch applied")
