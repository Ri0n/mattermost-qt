#include <QtTest>

#include <QJsonObject>

#include "backend/types/BackendUser.h"

using namespace Mattermost;

namespace {

QJsonObject userJson(qulonglong pictureVersion, qulonglong updateAt,
                     const QString& firstName = QStringLiteral("Alice"))
{
    return QJsonObject {
        {QStringLiteral("id"), QStringLiteral("user-id")},
        {QStringLiteral("create_at"), 1},
        {QStringLiteral("update_at"), static_cast<qint64>(updateAt)},
        {QStringLiteral("delete_at"), 0},
        {QStringLiteral("username"), QStringLiteral("alice")},
        {QStringLiteral("email"), QStringLiteral("alice@example.invalid")},
        {QStringLiteral("first_name"), firstName},
        {QStringLiteral("last_name"), QStringLiteral("Example")},
        {QStringLiteral("last_picture_update"), static_cast<qint64>(pictureVersion)},
        {QStringLiteral("notify_props"), QJsonObject {}},
        {QStringLiteral("props"), QJsonObject {}},
        {QStringLiteral("timezone"), QJsonObject {}},
    };
}

} // namespace

class BackendUserTest : public QObject
{
    Q_OBJECT

private slots:
    void parsesPictureVersion()
    {
        BackendUser user(userJson(1234, 100));

        QCOMPARE(user.last_picture_update, uint64_t(1234));
        QCOMPARE(user.avatar_picture_update, uint64_t(0));
    }

    void profileUpdateChangesPictureVersionButNotLoadedAvatarVersion()
    {
        BackendUser user(userJson(1234, 100));
        user.avatar_picture_update = 1234;

        BackendUser updated(userJson(5678, 200, QStringLiteral("Alicia")));
        QString changes;
        user.updateFrom(updated, changes);

        QCOMPARE(user.last_picture_update, uint64_t(5678));
        QCOMPARE(user.avatar_picture_update, uint64_t(1234));
        QCOMPARE(user.update_at, uint64_t(200));
        QCOMPARE(user.first_name, QStringLiteral("Alicia"));
        QVERIFY(changes.contains(QStringLiteral("last_picture_update")));
    }
};

QTEST_MAIN(BackendUserTest)

#include "BackendUserTest.moc"
