#include <QtTest>

#include "chat-area/ThreadNavigationPlacement.h"

using Mattermost::ThreadNavigationPlacement;

class ThreadNavigationPlacementTest : public QObject
{
    Q_OBJECT

private slots:
    void estimatedTargetBlocksMaterialization()
    {
        ThreadNavigationPlacement placement;
        placement.trackEstimated(QStringLiteral("target"), 12);

        QVERIFY(placement.isActive());
        QCOMPARE(placement.postId(), QStringLiteral("target"));
        QCOMPARE(placement.index(), 12);
        QVERIFY(placement.blocksMaterialization(QStringLiteral("target")));
        QVERIFY(!placement.blocksMaterialization(QStringLiteral("other")));
    }

    void repeatedLookupKeepsEstimatedTargetBlocked()
    {
        ThreadNavigationPlacement placement;
        placement.trackEstimated(QStringLiteral("target"), 12);
        placement.trackExistingProvisional(QStringLiteral("target"), 12);

        QVERIFY(placement.isActive());
        QCOMPARE(placement.index(), 12);
        QVERIFY(placement.blocksMaterialization(QStringLiteral("target")));
    }

    void differentExactIndexReleasesEstimatedTarget()
    {
        ThreadNavigationPlacement placement;
        placement.trackEstimated(QStringLiteral("target"), 12);

        const auto confirmation = placement.confirmExactWindow(
            20,
            QStringList { QStringLiteral("before"),
                          QStringLiteral("target"),
                          QStringLiteral("after") });

        QVERIFY(confirmation.isValid());
        QVERIFY(confirmation.wasEstimated);
        QVERIFY(confirmation.moved());
        QCOMPARE(confirmation.previousIndex, 12);
        QCOMPARE(confirmation.authoritativeIndex, 21);
        QVERIFY(!placement.isActive());
        QVERIFY(!placement.blocksMaterialization(QStringLiteral("target")));
    }

    void sameExactIndexStillReleasesEstimatedTarget()
    {
        ThreadNavigationPlacement placement;
        placement.trackEstimated(QStringLiteral("target"), 21);

        const auto confirmation = placement.confirmExactWindow(
            20,
            QStringList { QStringLiteral("before"),
                          QStringLiteral("target"),
                          QStringLiteral("after") });

        QVERIFY(confirmation.isValid());
        QVERIFY(confirmation.wasEstimated);
        QVERIFY(!confirmation.moved());
        QCOMPARE(confirmation.previousIndex, 21);
        QCOMPARE(confirmation.authoritativeIndex, 21);
        QVERIFY(!placement.isActive());
        QVERIFY(!placement.blocksMaterialization(QStringLiteral("target")));
    }

    void unrelatedExactWindowKeepsEstimatedTargetBlocked()
    {
        ThreadNavigationPlacement placement;
        placement.trackEstimated(QStringLiteral("target"), 12);

        const auto confirmation = placement.confirmExactWindow(
            20,
            QStringList { QStringLiteral("first"), QStringLiteral("second") });

        QVERIFY(!confirmation.isValid());
        QVERIFY(placement.isActive());
        QCOMPARE(placement.index(), 12);
        QVERIFY(placement.blocksMaterialization(QStringLiteral("target")));
    }

    void cachedProvisionalTargetRemainsMaterializable()
    {
        ThreadNavigationPlacement placement;
        placement.trackExistingProvisional(QStringLiteral("target"), 7);

        QVERIFY(placement.isActive());
        QVERIFY(!placement.blocksMaterialization(QStringLiteral("target")));

        const auto confirmation = placement.confirmExactWindow(
            6,
            QStringList { QStringLiteral("before"), QStringLiteral("target") });

        QVERIFY(confirmation.isValid());
        QVERIFY(!confirmation.wasEstimated);
        QCOMPARE(confirmation.previousIndex, 7);
        QCOMPARE(confirmation.authoritativeIndex, 7);
        QVERIFY(!placement.isActive());
    }
};

QTEST_APPLESS_MAIN(ThreadNavigationPlacementTest)

#include "ThreadNavigationPlacementTest.moc"
