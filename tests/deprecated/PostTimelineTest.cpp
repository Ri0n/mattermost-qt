#include <QtTest>

#include "backend/PostTimeline.h"
#include "backend/TimelineSeekState.h"

using namespace Mattermost;

class PostTimelineTest : public QObject
{
    Q_OBJECT

private slots:
    void representsUnloadedRangesAsGaps()
    {
        PostTimeline timeline(100);
        timeline.reset(10);
        timeline.placeWindow(7, {QStringLiteral("p7"), QStringLiteral("p8"), QStringLiteral("p9")});

        const auto spans = timeline.spans();
        QCOMPARE(spans.size(), 2);
        QCOMPARE(spans.at(0).kind, PostTimeline::GapSpan);
        QCOMPARE(spans.at(0).firstIndex, 0);
        QCOMPARE(spans.at(0).count, 7);
        QCOMPARE(spans.at(0).estimatedHeight, qint64(700));
        QCOMPARE(spans.at(1).kind, PostTimeline::LoadedSpan);
        QCOMPARE(spans.at(1).firstIndex, 7);
        QCOMPARE(spans.at(1).postIds,
                 QStringList({QStringLiteral("p7"), QStringLiteral("p8"), QStringLiteral("p9")}));
        QCOMPARE(timeline.estimatedTotalHeight(), qint64(1000));
    }

    void mapsPixelsAcrossLoadedAndEstimatedRows()
    {
        PostTimeline timeline(100);
        timeline.reset(6);
        timeline.placeWindow(2, {QStringLiteral("p2"), QStringLiteral("p3")});
        timeline.recordMeasuredHeight(QStringLiteral("p2"), 50);
        timeline.recordMeasuredHeight(QStringLiteral("p3"), 150);

        QCOMPARE(timeline.estimatedPixelForIndex(2), qint64(200));
        QCOMPARE(timeline.estimatedPixelForIndex(3), qint64(250));
        QCOMPARE(timeline.estimatedPixelForIndex(4), qint64(400));
        QCOMPARE(timeline.estimatedTotalHeight(), qint64(600));

        auto location = timeline.locatePixel(225);
        QVERIFY(location.loaded);
        QCOMPARE(location.logicalIndex, 2);
        QCOMPARE(location.postId, QStringLiteral("p2"));
        QCOMPARE(location.offsetWithinRow, 25);

        location = timeline.locatePixel(450);
        QVERIFY(!location.loaded);
        QCOMPARE(location.logicalIndex, 4);
        QCOMPARE(location.offsetWithinRow, 50);
    }

    void relocatesDuplicateIdsInsteadOfDuplicatingThem()
    {
        PostTimeline timeline;
        timeline.reset(8);
        timeline.placeWindow(5, {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")});
        QCOMPARE(timeline.loadedCount(), 3);

        timeline.placeWindow(2, {QStringLiteral("x"), QStringLiteral("b"), QStringLiteral("y")});

        QCOMPARE(timeline.loadedCount(), 5);
        QCOMPARE(timeline.indexOf(QStringLiteral("b")), 3);
        QCOMPARE(timeline.postIdAt(6), QString());
        QCOMPARE(timeline.postIdAt(3), QStringLiteral("b"));
    }

    void authoritativeWindowReplacesIndexCollision()
    {
        PostTimeline timeline;
        timeline.reset(5);
        timeline.placeWindow(1, {QStringLiteral("old")});
        timeline.recordMeasuredHeight(QStringLiteral("old"), 300);

        timeline.placeWindow(1, {QStringLiteral("new")});
        QCOMPARE(timeline.loadedCount(), 1);
        QVERIFY(!timeline.contains(QStringLiteral("old")));
        QCOMPARE(timeline.postIdAt(1), QStringLiteral("new"));
    }

    void resizePreservingNewestMovesCapacityAtOldestEdge()
    {
        PostTimeline timeline;
        timeline.reset(160);
        timeline.placeWindow(80, {QStringLiteral("newest-a"), QStringLiteral("newest-b")});
        timeline.recordMeasuredHeight(QStringLiteral("newest-a"), 123);

        timeline.setTotalCountPreservingNewest(240);
        QCOMPARE(timeline.indexOf(QStringLiteral("newest-a")), 160);
        QCOMPARE(timeline.indexOf(QStringLiteral("newest-b")), 161);
        QCOMPARE(timeline.loadedCount(), 2);

        timeline.setTotalCountPreservingNewest(100);
        QCOMPARE(timeline.indexOf(QStringLiteral("newest-a")), 20);
        QCOMPARE(timeline.indexOf(QStringLiteral("newest-b")), 21);
        QCOMPARE(timeline.loadedCount(), 2);
        QCOMPARE(timeline.estimatedRowHeight(), 123);
    }

    void newestRelativeGapAnchorSurvivesOldestGrowth()
    {
        PostTimeline timeline(100);
        timeline.reset(160);
        timeline.placeWindow(80, {QStringLiteral("tail-a"), QStringLiteral("tail-b")});

        const auto before = timeline.locatePixel(25 * 100 + 40);
        QVERIFY(before.isValid());
        QVERIFY(!before.loaded);
        const int distanceFromNewest = timeline.totalCount() - 1 - before.logicalIndex;
        const int offset = before.offsetWithinRow;

        timeline.setTotalCountPreservingNewest(240);
        QCOMPARE(timeline.postIdAt(timeline.indexOf(QStringLiteral("tail-a"))),
                 QStringLiteral("tail-a"));

        const int restoredIndex = timeline.totalCount() - 1 - distanceFromNewest;
        const qint64 restoredPixel = timeline.estimatedPixelForIndex(restoredIndex) + offset;
        const auto after = timeline.locatePixel(restoredPixel);

        QVERIFY(after.isValid());
        QCOMPARE(timeline.totalCount() - 1 - after.logicalIndex, distanceFromNewest);
        QCOMPARE(after.offsetWithinRow, offset);
    }

    void cursorPrependOverlapDoesNotMoveExistingRows()
    {
        PostTimeline timeline;
        timeline.reset(20);
        timeline.placeWindow(10, {QStringLiteral("p10"), QStringLiteral("p11"), QStringLiteral("p12")});

        timeline.placeWindow(8, {QStringLiteral("p8"), QStringLiteral("p9")});

        QCOMPARE(timeline.indexOf(QStringLiteral("p10")), 10);
        QCOMPARE(timeline.indexOf(QStringLiteral("p11")), 11);
        QCOMPARE(timeline.indexOf(QStringLiteral("p12")), 12);
        QCOMPARE(timeline.loadedCount(), 5);
        QCOMPARE(timeline.spans().at(1).postIds,
                 QStringList({QStringLiteral("p8"), QStringLiteral("p9"),
                              QStringLiteral("p10"), QStringLiteral("p11"),
                              QStringLiteral("p12")}));
    }

    void cursorAppendOverlapDoesNotMoveExistingRows()
    {
        PostTimeline timeline;
        timeline.reset(20);
        timeline.placeWindow(5, {QStringLiteral("p5"), QStringLiteral("p6"), QStringLiteral("p7")});

        timeline.placeWindow(8, {QStringLiteral("p8"), QStringLiteral("p9")});

        QCOMPARE(timeline.indexOf(QStringLiteral("p5")), 5);
        QCOMPARE(timeline.indexOf(QStringLiteral("p7")), 7);
        QCOMPARE(timeline.indexOf(QStringLiteral("p8")), 8);
        QCOMPARE(timeline.indexOf(QStringLiteral("p9")), 9);
        QCOMPARE(timeline.loadedCount(), 5);
    }

    void confirmedOldestBoundaryRemovesLeadingGap()
    {
        PostTimeline timeline;
        timeline.reset(100);
        timeline.placeWindow(35, {QStringLiteral("oldest"), QStringLiteral("p1"),
                                  QStringLiteral("p2"), QStringLiteral("p3")});

        QVERIFY(timeline.alignLoadedSpanToBoundary(QStringLiteral("oldest"), true));
        QCOMPARE(timeline.indexOf(QStringLiteral("oldest")), 0);
        QCOMPARE(timeline.indexOf(QStringLiteral("p3")), 3);

        const auto spans = timeline.spans();
        QCOMPARE(spans.first().kind, PostTimeline::LoadedSpan);
        QCOMPARE(spans.first().firstIndex, 0);
        QCOMPARE(spans.first().count, 4);
        QCOMPARE(spans.at(1).kind, PostTimeline::GapSpan);
        QCOMPARE(spans.at(1).firstIndex, 4);
    }

    void confirmedNewestBoundaryMovesUncertaintyBeforeLoadedSpan()
    {
        PostTimeline timeline;
        timeline.reset(100);
        timeline.placeWindow(35, {QStringLiteral("p0"), QStringLiteral("p1"),
                                  QStringLiteral("p2"), QStringLiteral("newest")});

        QVERIFY(timeline.alignLoadedSpanToBoundary(QStringLiteral("newest"), false));
        QCOMPARE(timeline.indexOf(QStringLiteral("p0")), 96);
        QCOMPARE(timeline.indexOf(QStringLiteral("newest")), 99);
        QCOMPARE(timeline.spans().last().kind, PostTimeline::LoadedSpan);
        QCOMPARE(timeline.spans().last().firstIndex, 96);
    }

    void prefetchesWhenFiveLoadedRowsRemainBeforeGap()
    {
        PostTimeline timeline;
        timeline.reset(100);
        QStringList ids;
        for (int i = 20; i < 50; ++i) {
            ids.push_back(QStringLiteral("p%1").arg(i));
        }
        timeline.placeWindow(20, ids);

        QCOMPARE(timeline.adjacentGapIndex(25, true, 5), 19);
        QCOMPARE(timeline.adjacentGapIndex(26, true, 5), -1);
        QCOMPARE(timeline.adjacentGapIndex(44, false, 5), 50);
        QCOMPARE(timeline.adjacentGapIndex(43, false, 5), -1);
    }

    void knownBoundaryNeverRequestsNonexistentGap()
    {
        PostTimeline timeline;
        timeline.reset(10);
        QStringList ids;
        for (int i = 0; i < 10; ++i) {
            ids.push_back(QStringLiteral("p%1").arg(i));
        }
        timeline.placeWindow(0, ids);

        QCOMPARE(timeline.adjacentGapIndex(0, true, 5), -1);
        QCOMPARE(timeline.adjacentGapIndex(9, false, 5), -1);
    }

    void liveTailAppendKeepsExistingIndicesAndGapStable()
    {
        PostTimeline timeline;
        timeline.reset(100);

        QStringList window;
        for (int i = 20; i < 50; ++i) {
            window.push_back(QStringLiteral("p%1").arg(i));
        }
        timeline.placeWindow(20, window);

        const auto before = timeline.spans();
        QCOMPARE(before.size(), 3);
        QCOMPARE(before.at(2).kind, PostTimeline::GapSpan);
        QCOMPARE(before.at(2).firstIndex, 50);
        QCOMPARE(before.at(2).count, 50);

        timeline.setTotalCount(101);
        timeline.placeWindow(100, {QStringLiteral("live")});

        QCOMPARE(timeline.indexOf(QStringLiteral("p20")), 20);
        QCOMPARE(timeline.indexOf(QStringLiteral("p49")), 49);
        QCOMPARE(timeline.indexOf(QStringLiteral("live")), 100);

        const auto after = timeline.spans();
        QCOMPARE(after.size(), 4);
        QCOMPARE(after.at(2).kind, PostTimeline::GapSpan);
        QCOMPARE(after.at(2).firstIndex, 50);
        QCOMPARE(after.at(2).count, 50);
        QCOMPARE(after.at(3).kind, PostTimeline::LoadedSpan);
        QCOMPARE(after.at(3).firstIndex, 100);
        QCOMPARE(after.at(3).postIds, QStringList({QStringLiteral("live")}));
    }

    void pruningKeepsNearestTwoHundredRowsAndVisibleCenter()
    {
        PostTimeline timeline;
        timeline.reset(260);
        QStringList ids;
        for (int i = 0; i < 260; ++i) {
            ids.push_back(QStringLiteral("p%1").arg(i));
        }
        timeline.placeWindow(0, ids);

        const QVector<int> removed = timeline.pruneLoadedToNearest(130, 200);
        QCOMPARE(timeline.loadedCount(), 200);
        QCOMPARE(removed.size(), 60);

        for (int i = 120; i <= 140; ++i) {
            QCOMPARE(timeline.postIdAt(i), QStringLiteral("p%1").arg(i));
        }

        const auto spans = timeline.spans();
        QCOMPARE(spans.first().kind, PostTimeline::GapSpan);
        QCOMPARE(spans.last().kind, PostTimeline::GapSpan);
    }

    void pruningSparseWindowsDropsFarthestMaterializationFirst()
    {
        PostTimeline timeline;
        timeline.reset(1000);

        QStringList oldWindow;
        QStringList visibleWindow;
        QStringList newestWindow;
        for (int i = 0; i < 90; ++i) {
            oldWindow.push_back(QStringLiteral("old%1").arg(i));
            visibleWindow.push_back(QStringLiteral("mid%1").arg(i));
            newestWindow.push_back(QStringLiteral("new%1").arg(i));
        }
        timeline.placeWindow(50, oldWindow);
        timeline.placeWindow(450, visibleWindow);
        timeline.placeWindow(850, newestWindow);

        timeline.pruneLoadedToNearest(495, 200);
        QCOMPARE(timeline.loadedCount(), 200);
        QVERIFY(timeline.contains(QStringLiteral("mid45")));

        for (const QString& id : visibleWindow) {
            QVERIFY(timeline.contains(id));
        }
    }

    void normalizedThumbPositionMapsToLogicalIndex()
    {
        PostTimeline timeline;
        timeline.reset(10000);

        QCOMPARE(timeline.logicalIndexForScrollPosition(0, 0, 1000), 0);
        QCOMPARE(timeline.logicalIndexForScrollPosition(500, 0, 1000), 5000);
        QCOMPARE(timeline.logicalIndexForScrollPosition(1000, 0, 1000), 9999);

        // Measured row heights refine gap geometry, but must not change the
        // meaning of a physical thumb fraction during random seek.
        timeline.placeWindow(100, {QStringLiteral("tall")});
        timeline.recordMeasuredHeight(QStringLiteral("tall"), 1000);
        QCOMPARE(timeline.logicalIndexForScrollPosition(500, 0, 1000), 5000);
    }

    void approximateSeekWindowNeverOverwritesLoadedSpan()
    {
        PostTimeline timeline;
        timeline.reset(100);
        QStringList loaded;
        for (int i = 40; i < 60; ++i) {
            loaded.push_back(QStringLiteral("p%1").arg(i));
        }
        timeline.placeWindow(40, loaded);

        const PostTimeline::LogicalWindow window = timeline.gapWindowNear(50, 10);
        QVERIFY(window.isValid());
        QVERIFY(window.lastIndex() < 40 || window.firstIndex >= 60);
        QCOMPARE(window.count, 10);
    }

    void seekGenerationRejectsStaleResponses()
    {
        TimelineSeekState state;
        state.setTarget(5000);
        state.markReady();
        const TimelineSeekState::Ticket oldTicket = state.currentTicket();
        QVERIFY(state.isCurrent(oldTicket));
        QVERIFY(state.isReady());

        state.setTarget(6000);
        QVERIFY(!state.isCurrent(oldTicket));
        QVERIFY(!state.isReady());

        state.markReady();
        const TimelineSeekState::Ticket current = state.currentTicket();
        QVERIFY(state.isCurrent(current));
        state.complete(current);
        QVERIFY(!state.isReady());
    }
};

QTEST_APPLESS_MAIN(PostTimelineTest)

#include "PostTimelineTest.moc"
