#include <QtTest>

#include "chat-area/ThreadTimelinePolicy.h"

using namespace Mattermost;

class ThreadTimelinePolicyTest : public QObject
{
    Q_OBJECT

private slots:
    void rootCountAlreadyIncludesLiveReply()
    {
        QCOMPARE(threadExpectedCountAfterLiveReply(101, 102, false), 102);
    }

    void liveSignalNeverAddsAnotherLogicalSlot()
    {
        // BackendChannel has already counted the reply before onNewPost. Equal
        // or temporarily lower metadata must therefore never synthesize +1.
        QCOMPARE(threadExpectedCountAfterLiveReply(101, 101, false), 101);
        QCOMPARE(threadExpectedCountAfterLiveReply(101, 99, false), 101);
    }

    void duplicateLiveReplyDoesNotGrowTimeline()
    {
        QCOMPARE(threadExpectedCountAfterLiveReply(101, 101, true), 101);
        QCOMPARE(threadExpectedCountAfterLiveReply(101, 102, true), 102);
    }

    void shortPageConfirmsNewestBoundary()
    {
        QVERIFY(threadPageConfirmsNewestBoundary(false, QString(), 7, 30));
        QVERIFY(threadPageConfirmsNewestBoundary(false, QString(), 0, 30));
        QVERIFY(!threadPageConfirmsNewestBoundary(false, QString(), 30, 30));
        QVERIFY(!threadPageConfirmsNewestBoundary(true, QString(), 7, 30));
        QVERIFY(!threadPageConfirmsNewestBoundary(false, QStringLiteral("next"), 7, 30));
    }

    void compactInitialPageAlreadyContainsNewest()
    {
        QVERIFY(threadInitialPageContainsNewest(1, 30));
        QVERIFY(threadInitialPageContainsNewest(30, 30));
        QVERIFY(!threadInitialPageContainsNewest(31, 30));
        QVERIFY(!threadInitialPageContainsNewest(500, 30));
    }

    void tailWindowIsPinnedToNewestLogicalEdge()
    {
        // root is logical row 0, so 30 newest replies in a 101-row thread occupy
        // exactly rows 71..100 and leave the unloaded history before them.
        QCOMPARE(threadTailWindowFirstIndex(101, 30), 71);
        QCOMPARE(threadTailWindowFirstIndex(61, 30), 31);
        QCOMPARE(threadTailWindowFirstIndex(31, 30), 1);
    }
};

QTEST_APPLESS_MAIN(ThreadTimelinePolicyTest)

#include "ThreadTimelinePolicyTest.moc"
