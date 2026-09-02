#include <QtTest>

#include "backend/PostTimeline.h"

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

        // A cursor API may repeat its boundary post. After stripping the cursor,
        // placing the older rows immediately before it must preserve the loaded
        // window's logical identity and produce one contiguous span.
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
};

QTEST_APPLESS_MAIN(PostTimelineTest)

#include "PostTimelineTest.moc"
