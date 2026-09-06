from pathlib import Path


def replace_once(path, old, new):
    p = Path(path)
    text = p.read_text()
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected one match, found {count}")
    p.write_text(text.replace(old, new, 1))


# PostCacheStore API + schema -------------------------------------------------
replace_once(
    "sources/backend/PostCacheStore.h",
    '''    /** Load a thread root plus newest cached replies. */\n    QJsonObject loadThread(const QString& channelId, const QString& rootId, int limit);\n\n    /** Invalidate a post whose durable raw representation is no longer trusted. */\n''',
    '''    /** Load a thread root plus newest cached replies. */\n    QJsonObject loadThread(const QString& channelId, const QString& rootId, int limit);\n\n    /**\n     * Persist one authoritative oldest->newest tail window. rootId is empty for\n     * the main channel timeline and non-empty for thread replies.\n     */\n    bool storeTailWindow(const QString& channelId,\n                         const QString& rootId,\n                         const QStringList& chronologicalPostIds);\n\n    /**\n     * Load the newest still-complete suffix of a previously observed tail\n     * window. Arbitrary cached rows that were never part of that window are\n     * deliberately ignored.\n     */\n    QJsonObject loadTailWindow(const QString& channelId,\n                               const QString& rootId,\n                               int limit);\n\n    /** Invalidate a post whose durable raw representation is no longer trusted. */\n''')
replace_once(
    "sources/backend/PostCacheStore.cpp",
    '#include <QDateTime>\n',
    '#include <QDateTime>\n#include <QJsonArray>\n')
replace_once(
    "sources/backend/PostCacheStore.cpp",
    'constexpr int SchemaVersion = 2;\n',
    'constexpr int SchemaVersion = 3;\n')
replace_once(
    "sources/backend/PostCacheStore.cpp",
    '''    if (!execStatement(QStringLiteral(\n            "CREATE TABLE IF NOT EXISTS channel_usage ("\n            " account_id INTEGER NOT NULL,"\n            " channel_id TEXT NOT NULL,"\n            " last_opened_at INTEGER NOT NULL,"\n            " PRIMARY KEY(account_id, channel_id),"\n            " FOREIGN KEY(account_id) REFERENCES accounts(id) ON DELETE CASCADE"\n            ") WITHOUT ROWID"))) {\n        return false;\n    }\n\n    if (!execStatement(QStringLiteral(\n''',
    '''    if (!execStatement(QStringLiteral(\n            "CREATE TABLE IF NOT EXISTS channel_usage ("\n            " account_id INTEGER NOT NULL,"\n            " channel_id TEXT NOT NULL,"\n            " last_opened_at INTEGER NOT NULL,"\n            " PRIMARY KEY(account_id, channel_id),"\n            " FOREIGN KEY(account_id) REFERENCES accounts(id) ON DELETE CASCADE"\n            ") WITHOUT ROWID"))) {\n        return false;\n    }\n\n    // Tail-window provenance is intentionally separate from post rows. A bag\n    // of cached rows does not prove adjacency because direct lookups, LRU\n    // eviction and reaction invalidation can create holes. The ordered ID list\n    // records only a server response that actually proved a newest-edge window.\n    if (!execStatement(QStringLiteral(\n            "CREATE TABLE IF NOT EXISTS tail_windows ("\n            " account_id INTEGER NOT NULL,"\n            " channel_id TEXT NOT NULL,"\n            " root_id TEXT NOT NULL DEFAULT '',"\n            " observed_at INTEGER NOT NULL,"\n            " post_ids BLOB NOT NULL,"\n            " PRIMARY KEY(account_id, channel_id, root_id),"\n            " FOREIGN KEY(account_id) REFERENCES accounts(id) ON DELETE CASCADE"\n            ") WITHOUT ROWID"))) {\n        return false;\n    }\n\n    if (!execStatement(QStringLiteral(\n''')

marker = '''bool PostCacheStore::removePost(const QString& postId)\n'''
insert = r'''bool PostCacheStore::storeTailWindow(const QString& channelId,
                                         const QString& rootId,
                                         const QStringList& chronologicalPostIds)
{
    if (!hasAccount() || channelId.isEmpty()
        || !isChannelEligible(channelId, nowMs())) {
        return false;
    }

    QJsonArray ids;
    QSet<QString> seen;
    for (const QString& postId : chronologicalPostIds) {
        if (postId.isEmpty() || seen.contains(postId)) {
            continue;
        }
        seen.insert(postId);
        ids.push_back(postId);
    }

    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "INSERT INTO tail_windows(account_id, channel_id, root_id, observed_at, post_ids) "
        "VALUES(?, ?, ?, ?, ?) "
        "ON CONFLICT(account_id, channel_id, root_id) DO UPDATE SET "
        "observed_at=excluded.observed_at, post_ids=excluded.post_ids"));
    query.addBindValue(accountId);
    query.addBindValue(channelId);
    query.addBindValue(rootId);
    query.addBindValue(nowMs());
    query.addBindValue(QJsonDocument(ids).toJson(QJsonDocument::Compact));
    if (!query.exec()) {
        qCWarning(lcPostCache) << "cannot record cached tail window"
                               << channelId << rootId << query.lastError().text();
        return false;
    }
    return true;
}

QJsonObject PostCacheStore::loadTailWindow(const QString& channelId,
                                           const QString& rootId,
                                           int limit)
{
    if (!hasAccount() || channelId.isEmpty() || limit <= 0
        || !isChannelEligible(channelId, nowMs())) {
        return {};
    }

    QSqlQuery window(database);
    window.prepare(QStringLiteral(
        "SELECT post_ids FROM tail_windows "
        "WHERE account_id=? AND channel_id=? AND root_id=?"));
    window.addBindValue(accountId);
    window.addBindValue(channelId);
    window.addBindValue(rootId);
    if (!window.exec() || !window.next()) {
        return {};
    }

    QJsonParseError error;
    const QJsonDocument encoded = QJsonDocument::fromJson(
        window.value(0).toByteArray(), &error);
    if (error.error != QJsonParseError::NoError || !encoded.isArray()) {
        qCWarning(lcPostCache) << "invalid cached tail-window provenance"
                               << channelId << rootId << error.errorString();
        return {};
    }

    const QJsonArray encodedIds = encoded.array();
    if (encodedIds.isEmpty()) {
        return {};
    }

    QStringList ids;
    ids.reserve(encodedIds.size());
    for (const QJsonValue& value : encodedIds) {
        const QString postId = value.toString();
        if (!postId.isEmpty()) {
            ids.push_back(postId);
        }
    }
    if (ids.isEmpty()) {
        return {};
    }

    // Only a suffix after the newest missing/corrupt row is still known to be
    // contiguous. This makes row-level invalidation and LRU eviction degrade the
    // window rather than silently closing a hole and inventing adjacency.
    const int firstCandidate = std::max(0, static_cast<int>(ids.size()) - limit);
    QJsonObject reversedSuffix;
    int firstUsable = static_cast<int>(ids.size());
    for (int index = static_cast<int>(ids.size()) - 1; index >= firstCandidate; --index) {
        const QString& postId = ids.at(index);
        const QJsonObject wrapped = loadPost(postId);
        const auto postIt = wrapped.constFind(postId);
        if (postIt == wrapped.constEnd() || !postIt->isObject()) {
            break;
        }
        const QJsonObject post = postIt->toObject();
        if (post.value(QStringLiteral("channel_id")).toString() != channelId
            || post.value(QStringLiteral("root_id")).toString() != rootId) {
            break;
        }
        reversedSuffix.insert(postId, post);
        firstUsable = index;
    }

    if (firstUsable == static_cast<int>(ids.size())) {
        return {};
    }

    QJsonObject result;
    for (int index = firstUsable; index < ids.size(); ++index) {
        const QString& postId = ids.at(index);
        const auto post = reversedSuffix.constFind(postId);
        if (post == reversedSuffix.constEnd()) {
            break;
        }
        result.insert(postId, *post);
    }
    return result;
}

'''
replace_once("sources/backend/PostCacheStore.cpp", marker, insert + marker)

replace_once(
    "sources/backend/PostCacheStore.cpp",
    '''    const bool changed = removePosts.numRowsAffected() > 0;\n\n    QSqlQuery removeUsage(database);\n''',
    '''    const bool changed = removePosts.numRowsAffected() > 0;\n\n    QSqlQuery removeWindows(database);\n    removeWindows.prepare(QStringLiteral(\n        "DELETE FROM tail_windows WHERE NOT EXISTS ("\n        " SELECT 1 FROM channel_usage u"\n        " WHERE u.account_id=tail_windows.account_id"\n        " AND u.channel_id=tail_windows.channel_id"\n        " AND u.last_opened_at>=?"\n        ")"));\n    removeWindows.addBindValue(cutoff);\n    if (!removeWindows.exec()) {\n        qCWarning(lcPostCache) << "cannot prune inactive tail windows"\n                               << removeWindows.lastError().text();\n    }\n\n    QSqlQuery removeUsage(database);\n''')

# Need QSet for storeTailWindow.
replace_once(
    "sources/backend/PostCacheStore.cpp",
    '#include <QSqlError>\n',
    '#include <QSet>\n#include <QSqlError>\n')

# PostCacheService API --------------------------------------------------------
replace_once(
    "sources/backend/PostCacheService.h",
    '#include <QString>\n',
    '#include <QString>\n#include <QStringList>\n')
replace_once(
    "sources/backend/PostCacheService.h",
    '''    /** Read a cached thread root plus newest replies asynchronously. */\n    void loadThread(const QString& server,\n                    const QString& userId,\n                    const QString& channelId,\n                    const QString& rootId,\n                    int limit,\n                    ReadCallback callback);\n\nprivate:\n''',
    '''    /** Read a cached thread root plus newest replies asynchronously. */\n    void loadThread(const QString& server,\n                    const QString& userId,\n                    const QString& channelId,\n                    const QString& rootId,\n                    int limit,\n                    ReadCallback callback);\n\n    /** Persist provenance for an authoritative newest main-channel window. */\n    void storeChannelTailWindow(const QString& server,\n                                const QString& userId,\n                                const QString& channelId,\n                                const QStringList& chronologicalPostIds);\n\n    /** Persist provenance for an authoritative newest thread-reply window. */\n    void storeThreadTailWindow(const QString& server,\n                               const QString& userId,\n                               const QString& channelId,\n                               const QString& rootId,\n                               const QStringList& chronologicalReplyIds);\n\n    /** Read the newest still-complete cached main-channel suffix. */\n    void loadChannelTailWindow(const QString& server,\n                               const QString& userId,\n                               const QString& channelId,\n                               int limit,\n                               ReadCallback callback);\n\n    /** Read the newest still-complete cached thread-reply suffix. */\n    void loadThreadTailWindow(const QString& server,\n                              const QString& userId,\n                              const QString& channelId,\n                              const QString& rootId,\n                              int limit,\n                              ReadCallback callback);\n\nprivate:\n''')

# Worker methods inserted before shutdown().
replace_once(
    "sources/backend/PostCacheService.cpp",
    '''    void shutdown()\n    {\n''',
    '''    void storeTailWindow(const QString& server,\n                         const QString& userId,\n                         const QString& channelId,\n                         const QString& rootId,\n                         const QStringList& chronologicalPostIds)\n    {\n        if (channelId.isEmpty() || !selectAccount(server, userId)) {\n            return;\n        }\n        store->storeTailWindow(channelId, rootId, chronologicalPostIds);\n    }\n\n    QJsonObject loadTailWindow(const QString& server,\n                               const QString& userId,\n                               const QString& channelId,\n                               const QString& rootId,\n                               int limit)\n    {\n        if (channelId.isEmpty() || limit <= 0 || !selectAccount(server, userId)) {\n            return {};\n        }\n        return store->loadTailWindow(channelId, rootId, limit);\n    }\n\n    void shutdown()\n    {\n''')

# Public service methods appended before namespace close.
replace_once(
    "sources/backend/PostCacheService.cpp",
    '''void PostCacheService::loadThread(const QString& server,\n                                  const QString& userId,\n                                  const QString& channelId,\n                                  const QString& rootId,\n                                  int limit,\n                                  ReadCallback callback)\n{\n''',
    '''void PostCacheService::loadThread(const QString& server,\n                                  const QString& userId,\n                                  const QString& channelId,\n                                  const QString& rootId,\n                                  int limit,\n                                  ReadCallback callback)\n{\n''')
# Insert after complete loadThread implementation by matching final tail.
needle = '''        Qt::QueuedConnection);\n}\n\n} // namespace Mattermost\n'''
addition = r'''        Qt::QueuedConnection);
}

void PostCacheService::storeChannelTailWindow(
    const QString& server,
    const QString& userId,
    const QString& channelId,
    const QStringList& chronologicalPostIds)
{
    if (!worker || server.trimmed().isEmpty() || userId.trimmed().isEmpty()
        || channelId.isEmpty()) {
        return;
    }
    PostCacheWorker* const currentWorker = worker;
    QMetaObject::invokeMethod(
        currentWorker,
        [currentWorker, server, userId, channelId, chronologicalPostIds] {
            currentWorker->storeTailWindow(server, userId, channelId, QString(),
                                           chronologicalPostIds);
        },
        Qt::QueuedConnection);
}

void PostCacheService::storeThreadTailWindow(
    const QString& server,
    const QString& userId,
    const QString& channelId,
    const QString& rootId,
    const QStringList& chronologicalReplyIds)
{
    if (!worker || server.trimmed().isEmpty() || userId.trimmed().isEmpty()
        || channelId.isEmpty() || rootId.isEmpty()) {
        return;
    }
    PostCacheWorker* const currentWorker = worker;
    QMetaObject::invokeMethod(
        currentWorker,
        [currentWorker, server, userId, channelId, rootId,
         chronologicalReplyIds] {
            currentWorker->storeTailWindow(server, userId, channelId, rootId,
                                           chronologicalReplyIds);
        },
        Qt::QueuedConnection);
}

void PostCacheService::loadChannelTailWindow(const QString& server,
                                             const QString& userId,
                                             const QString& channelId,
                                             int limit,
                                             ReadCallback callback)
{
    QPointer<QObject> context(&callbackContext);
    if (!worker || server.trimmed().isEmpty() || userId.trimmed().isEmpty()
        || channelId.isEmpty() || limit <= 0) {
        if (callback) {
            QMetaObject::invokeMethod(&callbackContext,
                                      [callback = std::move(callback)]() mutable {
                                          callback({});
                                      },
                                      Qt::QueuedConnection);
        }
        return;
    }
    PostCacheWorker* const currentWorker = worker;
    QMetaObject::invokeMethod(
        currentWorker,
        [currentWorker, server, userId, channelId, limit, context,
         callback = std::move(callback)]() mutable {
            QJsonObject result = currentWorker->loadTailWindow(
                server, userId, channelId, QString(), limit);
            if (!context || !callback) {
                return;
            }
            QMetaObject::invokeMethod(
                context.data(),
                [callback = std::move(callback), result = std::move(result)]() mutable {
                    callback(std::move(result));
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

void PostCacheService::loadThreadTailWindow(const QString& server,
                                            const QString& userId,
                                            const QString& channelId,
                                            const QString& rootId,
                                            int limit,
                                            ReadCallback callback)
{
    QPointer<QObject> context(&callbackContext);
    if (!worker || server.trimmed().isEmpty() || userId.trimmed().isEmpty()
        || channelId.isEmpty() || rootId.isEmpty() || limit <= 0) {
        if (callback) {
            QMetaObject::invokeMethod(&callbackContext,
                                      [callback = std::move(callback)]() mutable {
                                          callback({});
                                      },
                                      Qt::QueuedConnection);
        }
        return;
    }
    PostCacheWorker* const currentWorker = worker;
    QMetaObject::invokeMethod(
        currentWorker,
        [currentWorker, server, userId, channelId, rootId, limit, context,
         callback = std::move(callback)]() mutable {
            QJsonObject result = currentWorker->loadTailWindow(
                server, userId, channelId, rootId, limit);
            if (!context || !callback) {
                return;
            }
            QMetaObject::invokeMethod(
                context.data(),
                [callback = std::move(callback), result = std::move(result)]() mutable {
                    callback(std::move(result));
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

} // namespace Mattermost
'''
replace_once("sources/backend/PostCacheService.cpp", needle, addition)

# Store tests ----------------------------------------------------------------
replace_once(
    "tests/PostCacheStoreTest.cpp",
    '''    void selectsChannelRootsAndThreadReplies();\n    void enforcesThreadAndGlobalLimits();\n''',
    '''    void selectsChannelRootsAndThreadReplies();\n    void tailWindowRequiresProvenanceAndKeepsCompleteSuffix();\n    void enforcesThreadAndGlobalLimits();\n''')
marker = '''void PostCacheStoreTest::enforcesThreadAndGlobalLimits()\n'''
new_test = r'''void PostCacheStoreTest::tailWindowRequiresProvenanceAndKeepsCompleteSuffix()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    PostCacheStore cache(directory.filePath(QStringLiteral("posts.sqlite3")));
    QVERIFY(cache.open());
    QVERIFY(cache.setAccount(QStringLiteral("https://chat.example"),
                             QStringLiteral("alice-id")));
    QVERIFY(cache.recordChannelOpened(QStringLiteral("c1"),
                                      QDateTime::currentMSecsSinceEpoch()));

    QJsonObject posts;
    insertPost(posts, makePost(QStringLiteral("r1"), QStringLiteral("c1"), QString(), 10));
    insertPost(posts, makePost(QStringLiteral("r2"), QStringLiteral("c1"), QString(), 20));
    insertPost(posts, makePost(QStringLiteral("r3"), QStringLiteral("c1"), QString(), 30));
    insertPost(posts, makePost(QStringLiteral("t1"), QStringLiteral("c1"),
                               QStringLiteral("r2"), 21));
    insertPost(posts, makePost(QStringLiteral("t2"), QStringLiteral("c1"),
                               QStringLiteral("r2"), 22));
    insertPost(posts, makePost(QStringLiteral("t3"), QStringLiteral("c1"),
                               QStringLiteral("r2"), 23));
    QCOMPARE(cache.storePosts(posts), posts.size());

    // A row bag alone never becomes an adjacency proof.
    QVERIFY(cache.loadTailWindow(QStringLiteral("c1"), QString(), 10).isEmpty());

    QVERIFY(cache.storeTailWindow(QStringLiteral("c1"), QString(),
                                  { QStringLiteral("r1"), QStringLiteral("r2"),
                                    QStringLiteral("r3") }));
    QJsonObject roots = cache.loadTailWindow(QStringLiteral("c1"), QString(), 10);
    QCOMPARE(roots.size(), 3);

    // Invalidating a middle row leaves only the newer contiguous suffix usable.
    QVERIFY(cache.removePost(QStringLiteral("r2")));
    roots = cache.loadTailWindow(QStringLiteral("c1"), QString(), 10);
    QCOMPARE(roots.size(), 1);
    QVERIFY(roots.contains(QStringLiteral("r3")));

    QVERIFY(cache.storeTailWindow(QStringLiteral("c1"), QStringLiteral("r2"),
                                  { QStringLiteral("t1"), QStringLiteral("t2"),
                                    QStringLiteral("t3") }));
    QJsonObject replies = cache.loadTailWindow(QStringLiteral("c1"),
                                               QStringLiteral("r2"), 2);
    QCOMPARE(replies.size(), 2);
    QVERIFY(replies.contains(QStringLiteral("t2")));
    QVERIFY(replies.contains(QStringLiteral("t3")));

    // Losing the newest row means the stored window can no longer seed a tail.
    QVERIFY(cache.removePost(QStringLiteral("t3")));
    QVERIFY(cache.loadTailWindow(QStringLiteral("c1"),
                                 QStringLiteral("r2"), 10).isEmpty());
}

'''
replace_once("tests/PostCacheStoreTest.cpp", marker, new_test + marker)

# Service test extends FIFO coverage to provenance writes/reads.
replace_once(
    "tests/PostCacheServiceTest.cpp",
    '''    service.storePosts(server, user, posts, 1);\n\n    const QJsonObject direct = waitForRead''',
    '''    service.storePosts(server, user, posts, 1);\n    service.storeChannelTailWindow(server, user, channel, { root });\n    service.storeThreadTailWindow(server, user, channel, root,\n                                  { QStringLiteral("reply") });\n\n    const QJsonObject direct = waitForRead''')
replace_once(
    "tests/PostCacheServiceTest.cpp",
    '''    QVERIFY(thread.contains(root));\n    QVERIFY(thread.contains(QStringLiteral("reply")));\n}\n''',
    '''    QVERIFY(thread.contains(root));\n    QVERIFY(thread.contains(QStringLiteral("reply")));\n\n    const QJsonObject channelTail = waitForRead([&](PostCacheService::ReadCallback callback) {\n        service.loadChannelTailWindow(server, user, channel, 10, std::move(callback));\n    });\n    QCOMPARE(channelTail.size(), 1);\n    QVERIFY(channelTail.contains(root));\n\n    const QJsonObject threadTail = waitForRead([&](PostCacheService::ReadCallback callback) {\n        service.loadThreadTailWindow(server, user, channel, root, 10, std::move(callback));\n    });\n    QCOMPARE(threadTail.size(), 1);\n    QVERIFY(threadTail.contains(QStringLiteral("reply")));\n}\n''')

print("tail-window provenance patch applied")
