#include <QtTest>

#include <QJsonArray>
#include <QJsonObject>

#include "backend/types/BackendPoll.h"

using namespace Mattermost;

class BackendPollTest : public QObject
{
    Q_OBJECT

private slots:
    void metadataUsesFullOptionIndices();
    void metadataUsesCurrentAdminPermissionKey();
    void definitionRefreshPreservesMetadata();
};

void BackendPollTest::metadataUsesFullOptionIndices()
{
    QJsonArray actions;
    for (int index = 0; index < 8; ++index) {
        QJsonObject action;
        action.insert(QStringLiteral("name"),
                      index == 7 ? QStringLiteral("selected-option")
                                 : QStringLiteral("option-%1").arg(index));
        // Put non-voting/admin actions before the selected answer. The metadata
        // index is still an index into the complete options array, not into a
        // filtered vector of voting buttons.
        action.insert(QStringLiteral("id"),
                      index < 3 ? QStringLiteral("admin-%1").arg(index)
                                : QStringLiteral("vote-%1").arg(index));
        actions.append(action);
    }

    QJsonObject pollJson;
    pollJson.insert(QStringLiteral("actions"), actions);
    BackendPoll poll(QStringLiteral("poll-id"), pollJson);

    QJsonObject metadata;
    metadata.insert(QStringLiteral("voted_answers"),
                    QJsonArray {QStringLiteral("selected-option")});
    poll.fillMetadata(metadata);

    QCOMPARE(poll.metadata.ownVoteOptions.size(), 1);
    QCOMPARE(poll.metadata.ownVoteOptions.constFirst(), uint32_t(7));
}

void BackendPollTest::metadataUsesCurrentAdminPermissionKey()
{
    QJsonObject pollJson;
    pollJson.insert(QStringLiteral("actions"), QJsonArray {
        QJsonObject {{QStringLiteral("name"), QStringLiteral("one")},
                     {QStringLiteral("id"), QStringLiteral("vote-one")}}
    });
    BackendPoll poll(QStringLiteral("poll-id"), pollJson);

    QJsonObject metadata;
    metadata.insert(QStringLiteral("can_manage_poll"), true);
    poll.fillMetadata(metadata);
    QVERIFY(poll.metadata.hasAdminPermissions);

    metadata.remove(QStringLiteral("can_manage_poll"));
    metadata.insert(QStringLiteral("admin_permission"), true);
    poll.fillMetadata(metadata);
    QVERIFY(poll.metadata.hasAdminPermissions);
}

void BackendPollTest::definitionRefreshPreservesMetadata()
{
    QJsonObject initialJson;
    initialJson.insert(QStringLiteral("title"), QStringLiteral("old title"));
    initialJson.insert(QStringLiteral("actions"), QJsonArray {
        QJsonObject {{QStringLiteral("name"), QStringLiteral("one")},
                     {QStringLiteral("id"), QStringLiteral("vote-one")}},
        QJsonObject {{QStringLiteral("name"), QStringLiteral("two")},
                     {QStringLiteral("id"), QStringLiteral("vote-two")}}
    });
    BackendPoll poll(QStringLiteral("poll-id"), initialJson);

    QJsonObject metadata;
    metadata.insert(QStringLiteral("can_manage_poll"), true);
    metadata.insert(QStringLiteral("voted_answers"),
                    QJsonArray {QStringLiteral("two")});
    poll.fillMetadata(metadata);

    QJsonObject refreshedJson;
    refreshedJson.insert(QStringLiteral("title"), QStringLiteral("new title"));
    refreshedJson.insert(QStringLiteral("text"), QStringLiteral("updated body"));
    refreshedJson.insert(QStringLiteral("fields"), QJsonArray {
        QJsonObject {{QStringLiteral("title"), QStringLiteral("one")},
                     {QStringLiteral("value"), QStringLiteral("1 vote")}},
        QJsonObject {{QStringLiteral("title"), QStringLiteral("two")},
                     {QStringLiteral("value"), QStringLiteral("2 votes")}}
    });
    BackendPoll refreshed(QStringLiteral("poll-id"), refreshedJson);

    poll.updateDefinition(refreshed);

    QCOMPARE(poll.title, QStringLiteral("new title"));
    QCOMPARE(poll.text, QStringLiteral("updated body"));
    QVERIFY(poll.hasEnded);
    QCOMPARE(poll.options.size(), 2);
    QCOMPARE(poll.options.at(1).voters, QStringLiteral("2 votes"));
    QVERIFY(poll.metadata.hasAdminPermissions);
    QCOMPARE(poll.metadata.ownVoteOptions.size(), 1);
    QCOMPARE(poll.metadata.ownVoteOptions.constFirst(), uint32_t(1));
}

QTEST_MAIN(BackendPollTest)
#include "BackendPollTest.moc"
