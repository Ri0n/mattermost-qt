#include <QtTest>

#include "backend/PostTimeline.h"
#include "backend/TimelineSeekState.h"

using namespace Mattermost;

class TimelineSeekPolicyTest : public QObject
{
    Q_OBJECT

private slots:
    void scrollbarFractionMapsToStableLogicalIndex()
    {
        PostTimeline timeline(100);
        timeline.reset(10000);

        QCOMPARE(timeline.logicalIndexForScrollPosition(0, 0, 1000), 0);
        QCOMPARE(timeline.logicalIndexForScrollPosition(500, 0, 1000), 5000);
        QCOMPARE(timeline.logicalIndexForScrollPosition(1000, 0, 1000), 9999);

        timeline.placeWindow(4995, {
            QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c"),
            QStringLiteral("d"), QStringLiteral("e"), QStringLiteral("f"),
            QStringLiteral("g"), QStringLiteral("h"), QStringLiteral("i"),
            QStringLiteral("j")
        });
        for (int i = 0; i < 10; ++i) {
            timeline.recordMeasuredHeight(timeline.postIdAt(4995 + i), 300 + i * 17);
        }

        // Refining sparse pixel geometry must not move a thumb-derived target.
        QCOMPARE(timeline.logicalIndexForScrollPosition(500, 0, 1000), 5000);
    }

    void loadedWindowMergesSeedAndEdges()
    {
        PostTimeline timeline;
        timeline.reset(100);
        timeline.placeWindow(50, {
            QStringLiteral("p50"), QStringLiteral("p51"), QStringLiteral("p52")
        });
        timeline.placeWindow(47, {
            QStringLiteral("p47"), QStringLiteral("p48"), QStringLiteral("p49")
        });
        timeline.placeWindow(53, {
            QStringLiteral("p53"), QStringLiteral("p54")
        });

        const auto window = timeline.loadedWindowContaining(51);
        QVERIFY(window.isValid());
        QCOMPARE(window.firstIndex, 47);
        QCOMPARE(window.lastIndex(), 54);
        QCOMPARE(window.count, 8);
    }

    void viewportCoverageHasMinimumAndSafetyCap()
    {
        PostTimeline timeline(96);
        timeline.reset(1000);
        QCOMPARE(timeline.rowsForViewportCoverage(800), 30);

        QStringList ids;
        for (int i = 0; i < 40; ++i) {
            ids.push_back(QStringLiteral("p%1").arg(i));
        }
        timeline.placeWindow(100, ids);
        for (const QString& id : ids) {
            timeline.recordMeasuredHeight(id, 20);
        }

        // Three 800px screens at 20px/row would request 120 rows, but a single
        // seek transaction is deliberately capped to avoid runaway fetches.
        QCOMPARE(timeline.rowsForViewportCoverage(800), 90);
    }

    void newerThumbTargetInvalidatesOldTicket()
    {
        TimelineSeekState state;
        state.setTarget(5000);
        state.markReady();
        const auto oldTicket = state.currentTicket();
        state.begin(oldTicket);
        QVERIFY(state.isActive(oldTicket));

        state.setTarget(7000);
        QVERIFY(!state.isCurrent(oldTicket));
        QVERIFY(!state.isActive(oldTicket));

        state.markReady();
        const auto newTicket = state.currentTicket();
        QVERIFY(state.isCurrent(newTicket));
        QCOMPARE(newTicket.targetIndex, 7000);
    }

    void stagedGrowthKeepsTargetBalanced()
    {
        TimelineSeekState state;
        state.setTarget(50);
        state.markReady();
        const auto ticket = state.currentTicket();
        state.begin(ticket);

        // An absolute 10-row server page may put TARGET at an edge rather than
        // at the centre. The state machine must compensate with edge loads.
        QCOMPARE(state.nextEdge(ticket, 50, 59, 30), TimelineSeekState::OlderEdge);
        QCOMPARE(state.nextEdge(ticket, 40, 59, 30), TimelineSeekState::NewerEdge);

        // Thirty rows in total are not sufficient if TARGET still has only ten
        // older rows. Keep growing the deficient side rather than exposing a gap
        // almost immediately above the intended centre.
        QCOMPARE(state.nextEdge(ticket, 40, 69, 30), TimelineSeekState::OlderEdge);
        QCOMPARE(state.nextEdge(ticket, 36, 69, 30), TimelineSeekState::NoEdge);
    }

    void provenBoundaryRedirectsExpansionToOtherSide()
    {
        TimelineSeekState state;
        state.setTarget(5);
        state.markReady();
        const auto ticket = state.currentTicket();
        state.begin(ticket);
        state.markBoundary(ticket, TimelineSeekState::OlderEdge);

        QCOMPARE(state.nextEdge(ticket, 0, 9, 30), TimelineSeekState::NewerEdge);
    }

    void expansionBudgetIsBounded()
    {
        TimelineSeekState state;
        state.setTarget(50);
        state.markReady();
        const auto ticket = state.currentTicket();
        state.begin(ticket);

        QVERIFY(state.noteExpansionRequest(ticket, 2));
        QVERIFY(state.noteExpansionRequest(ticket, 2));
        QVERIFY(!state.noteExpansionRequest(ticket, 2));
    }
};

QTEST_APPLESS_MAIN(TimelineSeekPolicyTest)

#include "TimelineSeekPolicyTest.moc"
