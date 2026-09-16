#include <QtTest>

#include "chat-area/post/PostSelectionPolicy.h"

using namespace Mattermost;

class PostSelectionPolicyTest : public QObject
{
    Q_OBJECT
private slots:
    void forwardAndBackwardRangesAreInclusive()
    {
        QCOMPARE(postSelectionRange(4, 7).first, 4);
        QCOMPARE(postSelectionRange(4, 7).last, 7);
        QCOMPARE(postSelectionRange(7, 4).first, 4);
        QCOMPARE(postSelectionRange(7, 4).last, 7);
    }

    void movingBackTowardAnchorShrinksSelection()
    {
        const auto expanded = postSelectionRange(10, 15);
        const auto shrunk = postSelectionRange(10, 12);
        QVERIFY(expanded.contains(15));
        QVERIFY(!shrunk.contains(15));
        QVERIFY(shrunk.contains(10));
        QVERIFY(shrunk.contains(12));
    }

    void clearingLastCheckboxExitsMode()
    {
        QVERIFY(postSelectionShouldExit(0));
        QVERIFY(postSelectionShouldExit(-1));
        QVERIFY(!postSelectionShouldExit(1));
    }
};

QTEST_APPLESS_MAIN(PostSelectionPolicyTest)
#include "PostSelectionPolicyTest.moc"
