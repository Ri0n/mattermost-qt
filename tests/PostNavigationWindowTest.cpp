#include <QtTest>

#include "backend/PostNavigationWindow.h"
#include "backend/PostWindowSelection.h"

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

    void fillsPinnedWindowWhenOnlyTwoNewerPostsExist()
    {
        QJsonArray before;
        for (int i = 1; i <= 30; ++i) {
            // Mattermost order: closest older post first, oldest last.
            before.push_back(QStringLiteral("o%1").arg(i));
        }
        const QJsonArray after {
            QStringLiteral("n2"), // newest first
            QStringLiteral("n1"),
        };

        const PostNavigationWindow reserve = buildPostNavigationWindow(
            after, QStringLiteral("target"), before);
        const QStringList visible = selectPostWindow(
            reserve.chronologicalIds, QStringLiteral("target"), 31);

        QCOMPARE(visible.size(), 31);
        QCOMPARE(visible.at(28), QStringLiteral("target"));
        QCOMPARE(visible.at(29), QStringLiteral("n1"));
        QCOMPARE(visible.at(30), QStringLiteral("n2"));
        QCOMPARE(visible.first(), QStringLiteral("o28"));
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
