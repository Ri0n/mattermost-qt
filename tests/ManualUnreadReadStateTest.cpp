#include <QtTest>

#include <QJsonObject>

#include "backend/Backend.h"
#include "backend/FollowingModel.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "backend/types/BackendTeam.h"

using namespace Mattermost;

namespace {

QJsonObject postJson(const QString& id,
                     quint64 createAt,
                     const QString& rootId = QString())
{
    return QJsonObject {
        {QStringLiteral("id"), id},
        {QStringLiteral("channel_id"), QStringLiteral("channel")},
        {QStringLiteral("root_id"), rootId},
        {QStringLiteral("user_id"), QStringLiteral("user")},
        {QStringLiteral("message"), id},
        {QStringLiteral("create_at"), static_cast<double>(createAt)},
        {QStringLiteral("update_at"), static_cast<double>(createAt)},
    };
}

BackendChannel* makeChannel(Backend& backend)
{
    Storage& storage = backend.getStorage();
    storage.addUser(QJsonObject {
        {QStringLiteral("id"), QStringLiteral("me")},
        {QStringLiteral("username"), QStringLiteral("me")},
    }, true);
    storage.addUser(QJsonObject {
        {QStringLiteral("id"), QStringLiteral("user")},
        {QStringLiteral("username"), QStringLiteral("user")},
    });

    BackendTeam* team = storage.addTeam(QJsonObject {
        {QStringLiteral("id"), QStringLiteral("team")},
        {QStringLiteral("name"), QStringLiteral("team")},
        {QStringLiteral("display_name"), QStringLiteral("Team")},
    });
    if (!team) {
        return nullptr;
    }

    return storage.addTeamChannel(*team, QJsonObject {
        {QStringLiteral("id"), QStringLiteral("channel")},
        {QStringLiteral("team_id"), QStringLiteral("team")},
        {QStringLiteral("type"), QStringLiteral("O")},
        {QStringLiteral("name"), QStringLiteral("channel")},
        {QStringLiteral("display_name"), QStringLiteral("Channel")},
        {QStringLiteral("last_post_at"), 400.0},
    });
}

} // namespace

class ManualUnreadReadStateTest : public QObject
{
    Q_OBJECT

private slots:
    void channelManualUnreadIsConsumedByReadThroughAtEnd()
    {
        Backend backend;
        BackendChannel* channel = makeChannel(backend);
        QVERIFY(channel);

        BackendPost* first = channel->addPost(postJson(QStringLiteral("post-1"), 100));
        BackendPost* second = channel->addPost(postJson(QStringLiteral("post-2"), 200));
        BackendPost* last = channel->addPost(postJson(QStringLiteral("post-3"), 300));
        QVERIFY(first);
        QVERIFY(second);
        QVERIFY(last);

        FollowingModel& model = FollowingModel::instance(backend);
        model.markPostUnread(channel->id, QString(), first->id, first->create_at);

        const FollowingModel::Entry* marked = model.findEntry(channel->id);
        QVERIFY(marked);
        QCOMPARE(marked->resumeState, FollowingModel::ResumeState::FirstUnread);
        QCOMPARE(marked->firstUnreadPostId, first->id);
        QVERIFY(marked->requiresAttention());

        // ChatLogWidget releases its manual-unread visibility gate with the
        // newest lower edge actually observed while the marker was away. Once
        // that high-water reaches the channel end, the local manual projection
        // must disappear rather than pinning Attention on the marked post.
        model.observeReadThrough(channel->id, QString(), *last, true);
        QVERIFY(!model.findEntry(channel->id));
    }

    void threadManualUnreadAdvancesPastTheMarker()
    {
        Backend backend;
        BackendChannel* channel = makeChannel(backend);
        QVERIFY(channel);

        BackendPost* root = channel->addPost(postJson(QStringLiteral("root"), 100));
        BackendPost* firstReply = channel->addPost(
            postJson(QStringLiteral("reply-1"), 200, root->id));
        BackendPost* secondReply = channel->addPost(
            postJson(QStringLiteral("reply-2"), 300, root->id));
        QVERIFY(root);
        QVERIFY(firstReply);
        QVERIFY(secondReply);

        FollowingModel& model = FollowingModel::instance(backend);
        model.markPostUnread(channel->id, root->id,
                             firstReply->id, firstReply->create_at);

        model.observeReadThrough(channel->id, root->id, *firstReply, false);
        const FollowingModel::Entry* afterMarker = model.findEntry(channel->id, root->id);
        QVERIFY(afterMarker);
        QCOMPARE(afterMarker->readThroughPostId, firstReply->id);
        QCOMPARE(afterMarker->resumeState, FollowingModel::ResumeState::FirstUnread);
        QCOMPARE(afterMarker->firstUnreadPostId, secondReply->id);

        model.observeReadThrough(channel->id, root->id, *secondReply, true);
        const FollowingModel::Entry* atEnd = model.findEntry(channel->id, root->id);
        QVERIFY(atEnd);
        QCOMPARE(atEnd->readThroughPostId, secondReply->id);
        QCOMPARE(atEnd->resumeState, FollowingModel::ResumeState::AtEnd);
        QVERIFY(atEnd->firstUnreadPostId.isEmpty());
    }
};

QTEST_MAIN(ManualUnreadReadStateTest)
#include "ManualUnreadReadStateTest.moc"
