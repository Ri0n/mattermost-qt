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
};

void BackendPollTest::metadataUsesFullOptionIndices()
{
    QJsonArray actions;
    for (int index = 0; index < 8; ++index) {
        QJsonObject action;
        action.insert(QStringLiteral("name"),
                      index == 7 ? QStringLiteral("8. vuoksa-08")
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
                    QJsonArray {QStringLiteral("8. vuoksa-08")});
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

QTEST_MAIN(BackendPollTest)
#include "BackendPollTest.moc"
