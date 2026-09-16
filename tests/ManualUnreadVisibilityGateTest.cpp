#include <QtTest>

#include "chat-area/ManualUnreadVisibilityGate.h"

using namespace Mattermost;

class ManualUnreadVisibilityGateTest : public QObject
{
    Q_OBJECT

private slots:
    void visibleMarkRequiresExitAndReentry()
    {
        ManualUnreadVisibilityGate gate;
        gate.markUnread(QStringLiteral("post"), true);

        QVERIFY(gate.update(true));
        QVERIFY(gate.update(true));
        QVERIFY(gate.update(false));
        QCOMPARE(gate.phase(), ManualUnreadVisibilityGate::Phase::WaitForEntry);
        QVERIFY(gate.update(false));
        QVERIFY(!gate.update(true));
        QVERIFY(!gate.active());
    }

    void initiallyHiddenMarkReadsOnFirstEntry()
    {
        ManualUnreadVisibilityGate gate;
        gate.markUnread(QStringLiteral("post"), false);

        QVERIFY(gate.update(false));
        QVERIFY(!gate.update(true));
        QVERIFY(!gate.active());
    }
};

QTEST_MAIN(ManualUnreadVisibilityGateTest)
#include "ManualUnreadVisibilityGateTest.moc"
