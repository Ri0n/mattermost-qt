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

        // Mean measured height is 100, so all gaps remain 100 px/row.
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
};

QTEST_APPLESS_MAIN(PostTimelineTest)

#include "PostTimelineTest.moc"
