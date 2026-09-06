#include <QtTest>

#include <QJsonObject>
#include <QTemporaryDir>

#include "backend/PostCacheStore.h"

using namespace Mattermost;

namespace {

QJsonObject makePost(const QString& id,
                     const QString& channelId,
                     const QString& rootId,
                     qint64 createAt,
                     const QString& message = QString())
{
    QJsonObject post;
    post.insert(QStringLiteral("id"), id);
    post.insert(QStringLiteral("channel_id"), channelId);
    post.insert(QStringLiteral("root_id"), rootId);
    post.insert(QStringLiteral("create_at"), createAt);
    post.insert(QStringLiteral("update_at"), createAt);
    post.insert(QStringLiteral("message"), message.isEmpty() ? id : message);
    post.insert(QStringLiteral("user_id"), QStringLiteral("user"));
    return post;
}

void insertPost(QJsonObject& posts, const QJsonObject& post)
{
    posts.insert(post.value(QStringLiteral("id")).toString(), post);
}

} // namespace

class PostCacheStoreTest final : public QObject
{
    Q_OBJECT
private slots:
    void roundTripAndAccountIsolation();
    void selectsChannelRootsAndThreadReplies();
    void enforcesThreadAndGlobalLimits();
};

void PostCacheStoreTest::roundTripAndAccountIsolation()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    PostCacheStore cache(directory.filePath(QStringLiteral("posts.sqlite3")));
    QVERIFY(cache.open());
    QVERIFY(cache.setAccount(QStringLiteral("https://chat.example/"),
                             QStringLiteral("alice-id")));

    const QString message = QStringLiteral("hello cache — compressed JSON survives round trip");
    QJsonObject posts;
    insertPost(posts, makePost(QStringLiteral("p1"), QStringLiteral("c1"),
                               QString(), 100, message));
    QCOMPARE(cache.storePosts(posts), 1);

    const QJsonObject loaded = cache.loadPost(QStringLiteral("p1"));
    QCOMPARE(loaded.value(QStringLiteral("p1")).toObject()
                 .value(QStringLiteral("message")).toString(), message);

    // Trailing slash normalization must resolve the same account.
    QVERIFY(cache.setAccount(QStringLiteral("https://chat.example"),
                             QStringLiteral("alice-id")));
    QVERIFY(!cache.loadPost(QStringLiteral("p1")).isEmpty());

    QVERIFY(cache.setAccount(QStringLiteral("https://chat.example"),
                             QStringLiteral("bob-id")));
    QVERIFY(cache.loadPost(QStringLiteral("p1")).isEmpty());

    QVERIFY(cache.setAccount(QStringLiteral("https://chat.example"),
                             QStringLiteral("alice-id")));
    QVERIFY(!cache.loadPost(QStringLiteral("p1")).isEmpty());
}

void PostCacheStoreTest::selectsChannelRootsAndThreadReplies()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    PostCacheStore cache(directory.filePath(QStringLiteral("posts.sqlite3")));
    QVERIFY(cache.open());
    QVERIFY(cache.setAccount(QStringLiteral("https://chat.example"),
                             QStringLiteral("alice-id")));

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
    insertPost(posts, makePost(QStringLiteral("other"), QStringLiteral("c2"), QString(), 40));
    QCOMPARE(cache.storePosts(posts), posts.size());

    const QJsonObject roots = cache.loadLatestChannelRoots(QStringLiteral("c1"), 2);
    QCOMPARE(roots.size(), 2);
    QVERIFY(roots.contains(QStringLiteral("r2")));
    QVERIFY(roots.contains(QStringLiteral("r3")));
    QVERIFY(!roots.contains(QStringLiteral("t3")));

    const QJsonObject thread = cache.loadThread(QStringLiteral("c1"),
                                                QStringLiteral("r2"), 3);
    QCOMPARE(thread.size(), 3);
    QVERIFY(thread.contains(QStringLiteral("r2")));
    QVERIFY(thread.contains(QStringLiteral("t2")));
    QVERIFY(thread.contains(QStringLiteral("t3")));
    QVERIFY(!thread.contains(QStringLiteral("t1")));
}

void PostCacheStoreTest::enforcesThreadAndGlobalLimits()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    PostCacheStore cache(directory.filePath(QStringLiteral("posts.sqlite3")));
    QVERIFY(cache.open());
    QVERIFY(cache.setAccount(QStringLiteral("https://chat.example"),
                             QStringLiteral("alice-id")));

    PostCacheStore::Limits limits;
    limits.maxBytes = 16 * 1024 * 1024;
    limits.maxPosts = 20;
    limits.maxPostsPerThread = 2;
    cache.setLimits(limits);

    QJsonObject threadPosts;
    insertPost(threadPosts, makePost(QStringLiteral("root"), QStringLiteral("c1"), QString(), 1));
    for (int i = 0; i < 5; ++i) {
        insertPost(threadPosts,
                   makePost(QStringLiteral("reply-%1").arg(i), QStringLiteral("c1"),
                            QStringLiteral("root"), 10 + i));
    }
    QCOMPARE(cache.storePosts(threadPosts), threadPosts.size());

    const QJsonObject thread = cache.loadThread(QStringLiteral("c1"),
                                                QStringLiteral("root"), 20);
    QCOMPARE(thread.size(), 3); // root + two retained replies
    QVERIFY(thread.contains(QStringLiteral("root")));

    limits.maxPosts = 3;
    limits.maxPostsPerThread = 20;
    cache.setLimits(limits);

    QJsonObject roots;
    for (int i = 0; i < 10; ++i) {
        insertPost(roots,
                   makePost(QStringLiteral("root-%1").arg(i), QStringLiteral("c1"),
                            QString(), 100 + i));
    }
    QCOMPARE(cache.storePosts(roots), roots.size());
    cache.maintenance();

    const PostCacheStore::Stats stats = cache.stats();
    QVERIFY(stats.postCount <= 3);
    QVERIFY(stats.payloadBytes > 0);
    QVERIFY(stats.databaseBytes > 0);
}

QTEST_APPLESS_MAIN(PostCacheStoreTest)
#include "PostCacheStoreTest.moc"
