#include "PostCacheStore.h"

#include <algorithm>
#include <utility>

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLoggingCategory>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <QVector>

namespace Mattermost {
namespace {

Q_LOGGING_CATEGORY(lcPostCache, "mattermost.cache.posts", QtWarningMsg)

constexpr int SchemaVersion = 2;
constexpr int InitialMaintenanceDelayMs = 15 * 1000;
constexpr int LruTouchGranularityMs = 60 * 1000;
constexpr int VacuumMinFreePages = 128;
constexpr int VacuumMaxPagesPerPass = 4096;
constexpr int EvictionBatchSize = 256;

QString normalizedServer(QString server)
{
    server = server.trimmed();
    while (server.endsWith(QLatin1Char('/'))) {
        server.chop(1);
    }
    return server;
}

} // namespace

PostCacheStore::PostCacheStore(QString path, QObject* parent)
    : QObject(parent)
    , databasePath(std::move(path))
    , connectionName(QStringLiteral("mattermost-post-cache-%1")
                         .arg(static_cast<qulonglong>(reinterpret_cast<quintptr>(this)),
                              0, 16))
{
    maintenanceTimer.setInterval(limits.maintenanceIntervalMs);
    maintenanceTimer.setTimerType(Qt::VeryCoarseTimer);
    connect(&maintenanceTimer, &QTimer::timeout, this, [this] {
        maintenance();
    });
}

PostCacheStore::~PostCacheStore()
{
    maintenanceTimer.stop();
    if (database.isValid()) {
        database.close();
    }
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);
}

bool PostCacheStore::open()
{
    if (database.isOpen()) {
        return true;
    }

    const QFileInfo fileInfo(databasePath);
    QDir parentDir = fileInfo.dir();
    if (!parentDir.exists() && !parentDir.mkpath(QStringLiteral("."))) {
        qCWarning(lcPostCache) << "cannot create post cache directory"
                               << parentDir.absolutePath();
        return false;
    }

    const bool newDatabase = !fileInfo.exists() || fileInfo.size() == 0;
    database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    database.setDatabaseName(databasePath);
    database.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
    if (!database.open()) {
        qCWarning(lcPostCache) << "cannot open post cache" << databasePath
                               << database.lastError().text();
        return false;
    }

    if (!initializeSchema(newDatabase)) {
        database.close();
        return false;
    }

    maintenanceTimer.start();
    QTimer::singleShot(InitialMaintenanceDelayMs, this, [this] {
        if (database.isOpen()) {
            maintenance();
        }
    });
    return true;
}

bool PostCacheStore::initializeSchema(bool newDatabase)
{
    if (newDatabase) {
        // auto_vacuum must be selected before the first table is created.
        if (!execStatement(QStringLiteral("PRAGMA page_size=4096"))
            || !execStatement(QStringLiteral("PRAGMA auto_vacuum=INCREMENTAL"))) {
            return false;
        }
    }

    if (!execStatement(QStringLiteral("PRAGMA journal_mode=WAL"))
        || !execStatement(QStringLiteral("PRAGMA synchronous=NORMAL"))
        || !execStatement(QStringLiteral("PRAGMA temp_store=MEMORY"))
        || !execStatement(QStringLiteral("PRAGMA foreign_keys=ON"))
        || !execStatement(QStringLiteral("PRAGMA busy_timeout=5000"))) {
        return false;
    }

    if (!execStatement(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS accounts ("
            " id INTEGER PRIMARY KEY,"
            " server TEXT NOT NULL,"
            " user_id TEXT NOT NULL,"
            " UNIQUE(server, user_id)"
            ")"))) {
        return false;
    }

    if (!execStatement(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS posts ("
            " account_id INTEGER NOT NULL,"
            " post_id TEXT NOT NULL,"
            " channel_id TEXT NOT NULL,"
            " root_id TEXT NOT NULL DEFAULT '',"
            " create_at INTEGER NOT NULL,"
            " update_at INTEGER NOT NULL,"
            " last_access INTEGER NOT NULL,"
            " payload BLOB NOT NULL,"
            " PRIMARY KEY(account_id, post_id),"
            " FOREIGN KEY(account_id) REFERENCES accounts(id) ON DELETE CASCADE"
            ") WITHOUT ROWID"))) {
        return false;
    }

    if (!execStatement(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS channel_usage ("
            " account_id INTEGER NOT NULL,"
            " channel_id TEXT NOT NULL,"
            " last_opened_at INTEGER NOT NULL,"
            " PRIMARY KEY(account_id, channel_id),"
            " FOREIGN KEY(account_id) REFERENCES accounts(id) ON DELETE CASCADE"
            ") WITHOUT ROWID"))) {
        return false;
    }

    if (!execStatement(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS posts_timeline_idx "
            "ON posts(account_id, channel_id, root_id, create_at, post_id)"))
        || !execStatement(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS posts_lru_idx ON posts(last_access)"))
        || !execStatement(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS channel_usage_open_idx "
            "ON channel_usage(account_id, last_opened_at)"))
        || !execStatement(QStringLiteral("PRAGMA user_version=%1").arg(SchemaVersion))) {
        return false;
    }

    return true;
}

bool PostCacheStore::setAccount(const QString& server, const QString& userId)
{
    if (!database.isOpen()) {
        return false;
    }

    const QString normalized = normalizedServer(server);
    const QString trimmedUserId = userId.trimmed();
    if (normalized.isEmpty() || trimmedUserId.isEmpty()) {
        accountId = -1;
        activeServer.clear();
        activeUserId.clear();
        return false;
    }

    if (accountId >= 0 && activeServer == normalized && activeUserId == trimmedUserId) {
        return true;
    }

    QSqlQuery insert(database);
    insert.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO accounts(server, user_id) VALUES(?, ?)"));
    insert.addBindValue(normalized);
    insert.addBindValue(trimmedUserId);
    if (!insert.exec()) {
        qCWarning(lcPostCache) << "cannot register cache account"
                               << insert.lastError().text();
        return false;
    }

    QSqlQuery select(database);
    select.prepare(QStringLiteral(
        "SELECT id FROM accounts WHERE server=? AND user_id=?"));
    select.addBindValue(normalized);
    select.addBindValue(trimmedUserId);
    if (!select.exec() || !select.next()) {
        qCWarning(lcPostCache) << "cannot resolve cache account"
                               << select.lastError().text();
        return false;
    }

    accountId = select.value(0).toLongLong();
    activeServer = normalized;
    activeUserId = trimmedUserId;
    return true;
}

bool PostCacheStore::recordChannelOpened(const QString& channelId, qint64 openedAt)
{
    if (!hasAccount() || channelId.isEmpty()) {
        return false;
    }

    const qint64 effectiveOpenedAt = openedAt > 0 ? openedAt : nowMs();
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "INSERT INTO channel_usage(account_id, channel_id, last_opened_at) "
        "VALUES(?, ?, ?) "
        "ON CONFLICT(account_id, channel_id) DO UPDATE SET "
        "last_opened_at=MAX(channel_usage.last_opened_at, excluded.last_opened_at)"));
    query.addBindValue(accountId);
    query.addBindValue(channelId);
    query.addBindValue(effectiveOpenedAt);
    if (!query.exec()) {
        qCWarning(lcPostCache) << "cannot record channel cache usage" << channelId
                               << query.lastError().text();
        return false;
    }
    return true;
}

int PostCacheStore::storePosts(const QJsonObject& postsObject)
{
    if (!hasAccount() || postsObject.isEmpty()) {
        return 0;
    }

    const qint64 timestamp = nowMs();
    QJsonObject eligiblePosts;
    for (auto it = postsObject.constBegin(); it != postsObject.constEnd(); ++it) {
        if (!it->isObject()) {
            continue;
        }
        const QJsonObject post = it->toObject();
        const QString postId = post.value(QStringLiteral("id")).toString(it.key());
        const QString channelId = post.value(QStringLiteral("channel_id")).toString();
        if (postId.isEmpty() || channelId.isEmpty()
            || !isChannelEligible(channelId, timestamp)) {
            continue;
        }
        eligiblePosts.insert(postId, post);
    }

    if (eligiblePosts.isEmpty()) {
        return 0;
    }

    if (!database.transaction()) {
        qCWarning(lcPostCache) << "cannot start cache write transaction"
                               << database.lastError().text();
        return 0;
    }

    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "INSERT INTO posts(account_id, post_id, channel_id, root_id, create_at, "
        "update_at, last_access, payload) VALUES(?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(account_id, post_id) DO UPDATE SET "
        "channel_id=excluded.channel_id, root_id=excluded.root_id, "
        "create_at=excluded.create_at, update_at=excluded.update_at, "
        "last_access=excluded.last_access, payload=excluded.payload"));

    int stored = 0;
    for (auto it = eligiblePosts.constBegin(); it != eligiblePosts.constEnd(); ++it) {
        const QJsonObject post = it->toObject();
        const QString postId = post.value(QStringLiteral("id")).toString(it.key());
        const QString channelId = post.value(QStringLiteral("channel_id")).toString();

        query.bindValue(0, accountId);
        query.bindValue(1, postId);
        query.bindValue(2, channelId);
        query.bindValue(3, post.value(QStringLiteral("root_id")).toString());
        query.bindValue(4, post.value(QStringLiteral("create_at")).toVariant().toLongLong());
        query.bindValue(5, post.value(QStringLiteral("update_at")).toVariant().toLongLong());
        query.bindValue(6, timestamp);
        query.bindValue(7, encodePost(post));
        if (!query.exec()) {
            qCWarning(lcPostCache) << "cannot cache post" << postId
                                   << query.lastError().text();
            database.rollback();
            return 0;
        }
        ++stored;
    }

    if (!database.commit()) {
        qCWarning(lcPostCache) << "cannot commit post cache transaction"
                               << database.lastError().text();
        database.rollback();
        return 0;
    }

    // Keep hard limits independent of the periodic vacuum timer. Vacuuming is
    // intentionally deferred; row eviction itself remains synchronous so the
    // cache cannot grow without bound between maintenance passes.
    pruneThreadLimits();
    pruneGlobalLimits();
    return stored;
}

QJsonObject PostCacheStore::loadPost(const QString& postId)
{
    if (!hasAccount() || postId.isEmpty()) {
        return {};
    }

    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "SELECT post_id, payload FROM posts WHERE account_id=? AND post_id=?"));
    query.addBindValue(accountId);
    query.addBindValue(postId);
    if (!query.exec()) {
        qCWarning(lcPostCache) << "cannot read cached post" << query.lastError().text();
        return {};
    }
    return readPostQuery(query, true);
}

QJsonObject PostCacheStore::loadLatestChannelRoots(const QString& channelId, int limit)
{
    if (!hasAccount() || channelId.isEmpty() || limit <= 0
        || !isChannelEligible(channelId, nowMs())) {
        return {};
    }

    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "SELECT post_id, payload FROM posts "
        "WHERE account_id=? AND channel_id=? AND root_id='' "
        "ORDER BY create_at DESC, post_id DESC LIMIT ?"));
    query.addBindValue(accountId);
    query.addBindValue(channelId);
    query.addBindValue(limit);
    if (!query.exec()) {
        qCWarning(lcPostCache) << "cannot read cached channel roots"
                               << query.lastError().text();
        return {};
    }
    return readPostQuery(query, true);
}

QJsonObject PostCacheStore::loadThread(const QString& channelId,
                                       const QString& rootId,
                                       int limit)
{
    if (!hasAccount() || channelId.isEmpty() || rootId.isEmpty() || limit <= 0
        || !isChannelEligible(channelId, nowMs())) {
        return {};
    }

    QJsonObject result;
    QSqlQuery rootQuery(database);
    rootQuery.prepare(QStringLiteral(
        "SELECT post_id, payload FROM posts "
        "WHERE account_id=? AND channel_id=? AND post_id=? LIMIT 1"));
    rootQuery.addBindValue(accountId);
    rootQuery.addBindValue(channelId);
    rootQuery.addBindValue(rootId);
    if (rootQuery.exec()) {
        const QJsonObject root = readPostQuery(rootQuery, true);
        for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
            result.insert(it.key(), *it);
        }
    }

    const int replyLimit = std::max(0, limit - (result.isEmpty() ? 0 : 1));
    if (replyLimit == 0) {
        return result;
    }

    QSqlQuery replies(database);
    replies.prepare(QStringLiteral(
        "SELECT post_id, payload FROM posts "
        "WHERE account_id=? AND channel_id=? AND root_id=? "
        "ORDER BY create_at DESC, post_id DESC LIMIT ?"));
    replies.addBindValue(accountId);
    replies.addBindValue(channelId);
    replies.addBindValue(rootId);
    replies.addBindValue(replyLimit);
    if (!replies.exec()) {
        qCWarning(lcPostCache) << "cannot read cached thread"
                               << replies.lastError().text();
        return result;
    }

    const QJsonObject cachedReplies = readPostQuery(replies, true);
    for (auto it = cachedReplies.constBegin(); it != cachedReplies.constEnd(); ++it) {
        result.insert(it.key(), *it);
    }
    return result;
}

bool PostCacheStore::removePost(const QString& postId)
{
    if (!hasAccount() || postId.isEmpty()) {
        return false;
    }

    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "DELETE FROM posts WHERE account_id=? AND post_id=?"));
    query.addBindValue(accountId);
    query.addBindValue(postId);
    if (!query.exec()) {
        qCWarning(lcPostCache) << "cannot invalidate cached post"
                               << query.lastError().text();
        return false;
    }
    return true;
}

void PostCacheStore::setLimits(const Limits& newLimits)
{
    limits.maxBytes = std::max<qint64>(1, newLimits.maxBytes);
    limits.maxPosts = std::max(1, newLimits.maxPosts);
    limits.maxPostsPerThread = std::max(1, newLimits.maxPostsPerThread);
    limits.maxChannelIdleMs = std::max<qint64>(1, newLimits.maxChannelIdleMs);
    limits.maintenanceIntervalMs = std::max(1000, newLimits.maintenanceIntervalMs);
    maintenanceTimer.setInterval(limits.maintenanceIntervalMs);
    if (database.isOpen()) {
        pruneInactiveChannels();
        pruneThreadLimits();
        pruneGlobalLimits();
    }
}

PostCacheStore::Stats PostCacheStore::stats() const
{
    Stats result;
    if (!database.isOpen()) {
        return result;
    }

    QSqlQuery query(database);
    if (query.exec(QStringLiteral(
            "SELECT COUNT(*), COALESCE(SUM(length(payload)), 0) FROM posts"))
        && query.next()) {
        result.postCount = query.value(0).toLongLong();
        result.payloadBytes = query.value(1).toLongLong();
    }

    result.databaseBytes = QFileInfo(databasePath).size();
    result.pageCount = pragmaValue(QStringLiteral("page_count"));
    result.freePages = pragmaValue(QStringLiteral("freelist_count"));
    return result;
}

void PostCacheStore::maintenance()
{
    if (!database.isOpen()) {
        return;
    }

    pruneInactiveChannels();
    pruneThreadLimits();
    pruneGlobalLimits();
    execStatement(QStringLiteral("PRAGMA optimize"));
    vacuumFreelist();
}

bool PostCacheStore::execStatement(const QString& sql) const
{
    QSqlQuery query(database);
    if (query.exec(sql)) {
        return true;
    }
    qCWarning(lcPostCache) << "SQLite statement failed" << sql
                           << query.lastError().text();
    return false;
}

qint64 PostCacheStore::pragmaValue(const QString& name) const
{
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("PRAGMA ") + name) || !query.next()) {
        return 0;
    }
    return query.value(0).toLongLong();
}

qint64 PostCacheStore::nowMs() const
{
    return QDateTime::currentMSecsSinceEpoch();
}

QByteArray PostCacheStore::encodePost(const QJsonObject& post) const
{
    return qCompress(QJsonDocument(post).toJson(QJsonDocument::Compact), 6);
}

QJsonObject PostCacheStore::decodePost(const QByteArray& payload) const
{
    const QByteArray json = qUncompress(payload);
    if (json.isEmpty()) {
        return {};
    }

    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        qCWarning(lcPostCache) << "invalid cached post payload" << error.errorString();
        return {};
    }
    return document.object();
}

QJsonObject PostCacheStore::readPostQuery(QSqlQuery& query, bool touchRows)
{
    QJsonObject result;
    QStringList touched;
    while (query.next()) {
        const QString postId = query.value(0).toString();
        const QJsonObject post = decodePost(query.value(1).toByteArray());
        if (postId.isEmpty() || post.isEmpty()) {
            continue;
        }
        result.insert(postId, post);
        if (touchRows) {
            touched.push_back(postId);
        }
    }
    query.finish();

    if (!touched.isEmpty()) {
        touchPosts(touched, nowMs());
    }
    return result;
}

bool PostCacheStore::touchPosts(const QStringList& postIds, qint64 timestamp)
{
    if (!hasAccount() || postIds.isEmpty()) {
        return true;
    }

    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "UPDATE posts SET last_access=? "
        "WHERE account_id=? AND post_id=? AND last_access<?"));
    const qint64 cutoff = timestamp - LruTouchGranularityMs;
    for (const QString& postId : postIds) {
        query.bindValue(0, timestamp);
        query.bindValue(1, accountId);
        query.bindValue(2, postId);
        query.bindValue(3, cutoff);
        if (!query.exec()) {
            qCWarning(lcPostCache) << "cannot touch cached post"
                                   << query.lastError().text();
            return false;
        }
    }
    return true;
}

bool PostCacheStore::isChannelEligible(const QString& channelId, qint64 timestamp) const
{
    if (!hasAccount() || channelId.isEmpty()) {
        return false;
    }

    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "SELECT last_opened_at FROM channel_usage "
        "WHERE account_id=? AND channel_id=?"));
    query.addBindValue(accountId);
    query.addBindValue(channelId);
    if (!query.exec() || !query.next()) {
        return false;
    }
    return query.value(0).toLongLong() >= timestamp - limits.maxChannelIdleMs;
}

bool PostCacheStore::pruneInactiveChannels()
{
    if (!database.isOpen()) {
        return false;
    }

    const qint64 cutoff = nowMs() - limits.maxChannelIdleMs;
    QSqlQuery removePosts(database);
    removePosts.prepare(QStringLiteral(
        "DELETE FROM posts WHERE NOT EXISTS ("
        " SELECT 1 FROM channel_usage u"
        " WHERE u.account_id=posts.account_id"
        " AND u.channel_id=posts.channel_id"
        " AND u.last_opened_at>=?"
        ")"));
    removePosts.addBindValue(cutoff);
    if (!removePosts.exec()) {
        qCWarning(lcPostCache) << "cannot prune inactive channel posts"
                               << removePosts.lastError().text();
        return false;
    }
    const bool changed = removePosts.numRowsAffected() > 0;

    QSqlQuery removeUsage(database);
    removeUsage.prepare(QStringLiteral(
        "DELETE FROM channel_usage WHERE last_opened_at<?"));
    removeUsage.addBindValue(cutoff);
    if (!removeUsage.exec()) {
        qCWarning(lcPostCache) << "cannot prune inactive channel usage"
                               << removeUsage.lastError().text();
    }
    return changed;
}

bool PostCacheStore::pruneThreadLimits()
{
    if (!database.isOpen()) {
        return false;
    }

    struct ThreadOverflow {
        qint64 account = -1;
        QString channel;
        QString root;
        int excess = 0;
    };

    QVector<ThreadOverflow> overflows;
    QSqlQuery select(database);
    select.prepare(QStringLiteral(
        "SELECT account_id, channel_id, root_id, COUNT(*) FROM posts "
        "WHERE root_id<>'' "
        "GROUP BY account_id, channel_id, root_id HAVING COUNT(*)>?"));
    select.addBindValue(limits.maxPostsPerThread);
    if (!select.exec()) {
        qCWarning(lcPostCache) << "cannot inspect thread cache limits"
                               << select.lastError().text();
        return false;
    }
    while (select.next()) {
        ThreadOverflow overflow;
        overflow.account = select.value(0).toLongLong();
        overflow.channel = select.value(1).toString();
        overflow.root = select.value(2).toString();
        overflow.excess = select.value(3).toInt() - limits.maxPostsPerThread;
        if (overflow.excess > 0) {
            overflows.push_back(std::move(overflow));
        }
    }
    select.finish();

    if (overflows.isEmpty()) {
        return false;
    }

    if (!database.transaction()) {
        return false;
    }
    for (const ThreadOverflow& overflow : overflows) {
        QSqlQuery remove(database);
        remove.prepare(QStringLiteral(
            "DELETE FROM posts WHERE account_id=? AND post_id IN ("
            " SELECT post_id FROM posts "
            " WHERE account_id=? AND channel_id=? AND root_id=? "
            " ORDER BY last_access ASC, create_at ASC, post_id ASC LIMIT ?"
            ")"));
        remove.addBindValue(overflow.account);
        remove.addBindValue(overflow.account);
        remove.addBindValue(overflow.channel);
        remove.addBindValue(overflow.root);
        remove.addBindValue(overflow.excess);
        if (!remove.exec()) {
            qCWarning(lcPostCache) << "cannot prune cached thread"
                                   << remove.lastError().text();
            database.rollback();
            return false;
        }
    }
    if (!database.commit()) {
        database.rollback();
        return false;
    }
    return true;
}

bool PostCacheStore::pruneGlobalLimits()
{
    if (!database.isOpen()) {
        return false;
    }

    QSqlQuery totals(database);
    if (!totals.exec(QStringLiteral(
            "SELECT COUNT(*), COALESCE(SUM(length(payload)), 0) FROM posts"))
        || !totals.next()) {
        return false;
    }

    qint64 count = totals.value(0).toLongLong();
    qint64 bytes = totals.value(1).toLongLong();
    totals.finish();
    if (count <= limits.maxPosts && bytes <= limits.maxBytes) {
        return false;
    }

    // Hysteresis avoids deleting one row on every subsequent insertion while
    // still treating maxPosts/maxBytes as hard upper bounds.
    const qint64 targetCount = count > limits.maxPosts
        ? std::max<qint64>(1, static_cast<qint64>(limits.maxPosts) * 95 / 100)
        : limits.maxPosts;
    const qint64 targetBytes = bytes > limits.maxBytes
        ? std::max<qint64>(1, limits.maxBytes * 95 / 100)
        : limits.maxBytes;

    bool changed = false;
    while (count > targetCount || bytes > targetBytes) {
        struct Candidate {
            qint64 account = -1;
            QString postId;
            qint64 bytes = 0;
        };
        QVector<Candidate> candidates;

        QSqlQuery oldest(database);
        oldest.prepare(QStringLiteral(
            "SELECT account_id, post_id, length(payload) FROM posts "
            "ORDER BY last_access ASC, create_at ASC, post_id ASC LIMIT ?"));
        oldest.addBindValue(EvictionBatchSize);
        if (!oldest.exec()) {
            qCWarning(lcPostCache) << "cannot select cache eviction candidates"
                                   << oldest.lastError().text();
            return changed;
        }
        while (oldest.next()) {
            candidates.push_back(Candidate {
                oldest.value(0).toLongLong(),
                oldest.value(1).toString(),
                oldest.value(2).toLongLong(),
            });
        }
        oldest.finish();
        if (candidates.isEmpty()) {
            break;
        }

        if (!database.transaction()) {
            return changed;
        }
        QSqlQuery remove(database);
        remove.prepare(QStringLiteral(
            "DELETE FROM posts WHERE account_id=? AND post_id=?"));

        int removed = 0;
        qint64 removedBytes = 0;
        for (const Candidate& candidate : candidates) {
            if (count - removed <= targetCount && bytes - removedBytes <= targetBytes) {
                break;
            }
            remove.bindValue(0, candidate.account);
            remove.bindValue(1, candidate.postId);
            if (!remove.exec()) {
                qCWarning(lcPostCache) << "cannot evict cached post"
                                       << remove.lastError().text();
                database.rollback();
                return changed;
            }
            ++removed;
            removedBytes += candidate.bytes;
        }

        if (!database.commit()) {
            database.rollback();
            return changed;
        }
        if (removed == 0) {
            break;
        }
        count -= removed;
        bytes -= removedBytes;
        changed = true;
    }
    return changed;
}

void PostCacheStore::vacuumFreelist()
{
    // WAL itself can otherwise retain old pages even after the main database is
    // compacted. TRUNCATE keeps the cache's on-disk footprint predictable.
    execStatement(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"));

    const qint64 pageCount = pragmaValue(QStringLiteral("page_count"));
    const qint64 freePages = pragmaValue(QStringLiteral("freelist_count"));
    if (pageCount <= 0 || freePages < VacuumMinFreePages
        || freePages * 100 < pageCount * 5) {
        return;
    }

    const qint64 pages = std::min<qint64>(freePages, VacuumMaxPagesPerPass);
    execStatement(QStringLiteral("PRAGMA incremental_vacuum(%1)").arg(pages));
    execStatement(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"));
}

} // namespace Mattermost
