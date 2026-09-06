#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>
#include <QStringList>
#include <QTimer>

namespace Mattermost {

/**
 * Persistent raw-post cache shared by all configured Mattermost accounts.
 *
 * Rows are account-scoped, compact-JSON encoded and compressed. The store owns
 * only durable payload/topology metadata; BackendChannel remains the resident
 * object model. Absolute server page provenance is deliberately not invented
 * from cached timestamps.
 */
class PostCacheStore final : public QObject
{
public:
    struct Limits {
        qint64 maxBytes = 5LL * 1024 * 1024 * 1024;
        int maxPosts = 10000;
        int maxPostsPerThread = 1000;
        qint64 maxChannelIdleMs = 10LL * 60 * 60 * 1000;
        int maintenanceIntervalMs = 10 * 60 * 1000;
    };

    struct Stats {
        qint64 postCount = 0;
        qint64 payloadBytes = 0;
        qint64 databaseBytes = 0;
        qint64 pageCount = 0;
        qint64 freePages = 0;
    };

    explicit PostCacheStore(QString databasePath, QObject* parent = nullptr);
    ~PostCacheStore() override;

    bool open();
    bool isOpen() const { return database.isOpen(); }

    /** Select the cache namespace for one server + Mattermost user id. */
    bool setAccount(const QString& server, const QString& userId);
    bool hasAccount() const { return accountId >= 0; }

    /** Record a user reading gesture for channel admission/retention policy. */
    bool recordChannelOpened(const QString& channelId, qint64 openedAt);

    /** Upsert full Mattermost post JSON objects keyed by their post id. */
    int storePosts(const QJsonObject& postsObject);

    /** Load one cached post and refresh its LRU access timestamp. */
    QJsonObject loadPost(const QString& postId);

    /** Load newest cached channel roots. Returned object is keyed by post id. */
    QJsonObject loadLatestChannelRoots(const QString& channelId, int limit);

    /** Load a thread root plus newest cached replies. */
    QJsonObject loadThread(const QString& channelId, const QString& rootId, int limit);

    /** Invalidate a post whose durable raw representation is no longer trusted. */
    bool removePost(const QString& postId);

    void setLimits(const Limits& newLimits);
    Limits currentLimits() const { return limits; }
    Stats stats() const;

    /** Enforce channel/LRU limits and reclaim SQLite free pages incrementally. */
    void maintenance();

private:
    bool initializeSchema(bool newDatabase);
    bool execStatement(const QString& sql) const;
    qint64 pragmaValue(const QString& name) const;
    qint64 nowMs() const;
    QByteArray encodePost(const QJsonObject& post) const;
    QJsonObject decodePost(const QByteArray& payload) const;
    QJsonObject readPostQuery(QSqlQuery& query, bool touchRows);
    bool touchPosts(const QStringList& postIds, qint64 timestamp);
    bool isChannelEligible(const QString& channelId, qint64 timestamp) const;
    bool pruneInactiveChannels();
    bool pruneThreadLimits();
    bool pruneGlobalLimits();
    void vacuumFreelist();

    QString databasePath;
    QString connectionName;
    QSqlDatabase database;
    qint64 accountId = -1;
    QString activeServer;
    QString activeUserId;
    Limits limits;
    QTimer maintenanceTimer;
};

} // namespace Mattermost
