#include <QtTest>

#include "chat-area/PostSourceRequestGate.h"

class PostSourceRequestGateTest : public QObject
{
    Q_OBJECT

private slots:
    void attachesOverlapAndImmediateNeighboursWithoutExpandingCoverage()
    {
        Mattermost::PostSourceRequestGate gate;
        gate.begin(130, 149, 130, 149);

        QVERIFY(gate.isActive());
        QCOMPARE(gate.expectedRange().first, 130);
        QCOMPARE(gate.expectedRange().last, 149);

        QVERIFY(gate.canAttach(135, 145));
        QVERIFY(gate.canAttach(120, 129));
        QVERIFY(gate.canAttach(150, 160));

        gate.attach(120, 129);
        gate.attach(150, 160);

        // Attachment only registers logical waiters. It must never grow the
        // transport coverage promised by the already-running request.
        QCOMPARE(gate.expectedRange().first, 130);
        QCOMPARE(gate.expectedRange().last, 149);
    }

    void distantDemandCanBypassInflightRequest()
    {
        Mattermost::PostSourceRequestGate gate;
        gate.begin(130, 149, 130, 149);

        QVERIFY(!gate.canAttach(100, 119));
        QVERIFY(!gate.canAttach(161, 170));
        QVERIFY(!gate.canAttach(170, 160));
    }

    void finishReleasesWaitersAndResetsState()
    {
        Mattermost::PostSourceRequestGate gate;
        gate.begin(130, 149, 130, 149);
        gate.attach(120, 129);
        gate.attach(150, 160);

        const QVector<Mattermost::PostSourceRequestGate::Range> waiters = gate.finish();
        QCOMPARE(waiters.size(), 3);
        QCOMPARE(waiters.at(0).first, 130);
        QCOMPARE(waiters.at(0).last, 149);
        QCOMPARE(waiters.at(1).first, 120);
        QCOMPARE(waiters.at(1).last, 129);
        QCOMPARE(waiters.at(2).first, 150);
        QCOMPARE(waiters.at(2).last, 160);

        QVERIFY(!gate.isActive());
        QVERIFY(!gate.expectedRange().isValid());
        QVERIFY(!gate.canAttach(130, 149));
        QVERIFY(gate.finish().isEmpty());
    }
};

QTEST_APPLESS_MAIN(PostSourceRequestGateTest)

#include "PostSourceRequestGateTest.moc"
