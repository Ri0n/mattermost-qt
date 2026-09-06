#include <QDateTime>
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
    service.storeChannelTailWindow(server, user, channel, { root });
    service.storeThreadTailWindow(server, user, channel, root,
                                  { QStringLiteral("reply") });

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

    const QJsonObject channelTail = waitForRead([&](PostCacheService::ReadCallback callback) {
        service.loadChannelTailWindow(server, user, channel, 10, std::move(callback));
    });
    QCOMPARE(channelTail.size(), 1);
    QVERIFY(channelTail.contains(root));

    const QJsonObject threadTail = waitForRead([&](PostCacheService::ReadCallback callback) {
        service.loadThreadTailWindow(server, user, channel, root, 10, std::move(callback));
    });
    QCOMPARE(threadTail.size(), 1);
    QVERIFY(threadTail.contains(QStringLiteral("reply")));
}

QTEST_MAIN(PostCacheServiceTest)
#include "PostCacheServiceTest.moc"
