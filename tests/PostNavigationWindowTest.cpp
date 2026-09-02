#include <QtTest>

#include "backend/PostNavigationWindow.h"

using namespace Mattermost;

class PostNavigationWindowTest : public QObject
{
    Q_OBJECT

private slots:
    void assemblesChronologicalWindow()
    {
        const QJsonArray after {
            QStringLiteral("newest"),
            QStringLiteral("newer"),
        };
        const QJsonArray before {
            QStringLiteral("older"),
            QStringLiteral("oldest"),
        };

        const PostNavigationWindow window = buildPostNavigationWindow(
            after, QStringLiteral("target"), before);

        QCOMPARE(window.newestFirstOrder,
                 QJsonArray({QStringLiteral("newest"), QStringLiteral("newer"),
                             QStringLiteral("target"), QStringLiteral("older"),
                             QStringLiteral("oldest")}));
        QCOMPARE(window.chronologicalIds,
                 QStringList({QStringLiteral("oldest"), QStringLiteral("older"),
                              QStringLiteral("target"), QStringLiteral("newer"),
                              QStringLiteral("newest")}));
    }

    void removesCursorAndTargetOverlap()
    {
        const QJsonArray after {
            QStringLiteral("n1"),
            QStringLiteral("target"),
        };
        const QJsonArray before {
            QStringLiteral("target"),
            QStringLiteral("o1"),
            QStringLiteral("o1"),
        };

        const PostNavigationWindow window = buildPostNavigationWindow(
            after, QStringLiteral("target"), before);

        QCOMPARE(window.newestFirstOrder,
                 QJsonArray({QStringLiteral("n1"), QStringLiteral("target"),
                             QStringLiteral("o1")}));
        QCOMPARE(window.chronologicalIds,
                 QStringList({QStringLiteral("o1"), QStringLiteral("target"),
                              QStringLiteral("n1")}));
    }

    void rejectsEmptyTarget()
    {
        const PostNavigationWindow window = buildPostNavigationWindow(
            QJsonArray({QStringLiteral("n1")}), QString(),
            QJsonArray({QStringLiteral("o1")}));

        QVERIFY(window.newestFirstOrder.isEmpty());
        QVERIFY(window.chronologicalIds.isEmpty());
    }
};

QTEST_APPLESS_MAIN(PostNavigationWindowTest)

#include "PostNavigationWindowTest.moc"
