#include <QtTest>

#include "backend/PostTimeline.h"
#include "chat-area/ThreadTimelinePolicy.h"

using namespace Mattermost;

class ThreadLiveTimelineTest : public QObject
{
    Q_OBJECT

private slots:
    void liveReplyKeepsExistingLogicalIndices()
    {
        PostTimeline timeline;
        timeline.reset(1000);

        QStringList existing;
        for (int i = 0; i < 200; ++i) {
            existing.push_back(QStringLiteral("p%1").arg(i));
        }
        timeline.placeWindow(0, existing);

        timeline.setTotalCount(1001);
        timeline.placeWindow(1000, {QStringLiteral("live")});

        QCOMPARE(timeline.loadedCount(), 201);
        for (int i = 0; i < 200; ++i) {
            QCOMPARE(timeline.indexOf(QStringLiteral("p%1").arg(i)), i);
        }
        QCOMPARE(timeline.indexOf(QStringLiteral("live")), 1000);
    }

    void liveReplyCrossingBudgetPrunesOnlyRemoteRows()
    {
        PostTimeline timeline;
        timeline.reset(1000);

        QStringList existing;
        for (int i = 0; i < 200; ++i) {
            existing.push_back(QStringLiteral("p%1").arg(i));
        }
        timeline.placeWindow(0, existing);

        // The websocket reply is a single new newest slot. With the viewport at
        // that newest edge, the 200-row budget should evict exactly the farthest
        // old row rather than relocating/rebuilding the retained 199 rows.
        timeline.setTotalCount(1001);
        timeline.placeWindow(1000, {QStringLiteral("live")});
        const QVector<int> removed = timeline.pruneLoadedToNearest(1000, 200);

        QCOMPARE(removed, QVector<int>({0}));
        QCOMPARE(timeline.loadedCount(), 200);
        QVERIFY(!timeline.contains(QStringLiteral("p0")));
        QVERIFY(timeline.contains(QStringLiteral("live")));
        QCOMPARE(timeline.indexOf(QStringLiteral("live")), 1000);

        for (int i = 1; i < 200; ++i) {
            QCOMPARE(timeline.indexOf(QStringLiteral("p%1").arg(i)), i);
        }

        const auto spans = timeline.spans();
        QCOMPARE(spans.size(), 4);
        QCOMPARE(spans.at(0).kind, PostTimeline::GapSpan);
        QCOMPARE(spans.at(0).firstIndex, 0);
        QCOMPARE(spans.at(0).count, 1);
        QCOMPARE(spans.at(1).kind, PostTimeline::LoadedSpan);
        QCOMPARE(spans.at(1).firstIndex, 1);
        QCOMPARE(spans.at(1).count, 199);
        QCOMPARE(spans.at(2).kind, PostTimeline::GapSpan);
        QCOMPARE(spans.at(2).firstIndex, 200);
        QCOMPARE(spans.at(2).count, 800);
        QCOMPARE(spans.at(3).kind, PostTimeline::LoadedSpan);
        QCOMPARE(spans.at(3).firstIndex, 1000);
        QCOMPARE(spans.at(3).postIds, QStringList({QStringLiteral("live")}));
    }

    void pruningNeverEvictsProtectedViewportMargin()
    {
        PostTimeline timeline;
        timeline.reset(1000);

        QStringList firstWindow;
        for (int i = 0; i < 140; ++i) {
            firstWindow.push_back(QStringLiteral("a%1").arg(i));
        }
        timeline.placeWindow(100, firstWindow); // 100..239

        QStringList secondWindow;
        for (int i = 0; i < 140; ++i) {
            secondWindow.push_back(QStringLiteral("b%1").arg(i));
        }
        timeline.placeWindow(500, secondWindow); // 500..639
        QCOMPARE(timeline.loadedCount(), 280);

        // Pretend the real viewport intersects logical rows 520..529. The UI
        // contract protects ten rows on each side, i.e. 510..539. Those rows
        // must survive even if a stale/simplified centre would otherwise make a
        // different subset look marginally closer.
        const QVector<int> removed = timeline.pruneLoadedToNearest(
            524, 200, 510, 539);

        QCOMPARE(timeline.loadedCount(), 200);
        for (int index = 510; index <= 539; ++index) {
            QVERIFY2(!timeline.postIdAt(index).isEmpty(),
                     qPrintable(QStringLiteral("protected logical row %1 was pruned").arg(index)));
        }
        for (int index : removed) {
            QVERIFY(index < 510 || index > 539);
        }
    }

    void backendReplyCountDoesNotCreateSecondLiveSlot()
    {
        // BackendChannel updates root.reply_count before emitting onNewPost.
        // The live handler therefore follows the reported count and never adds
        // an extra speculative slot for the same websocket reply.
        QCOMPARE(threadExpectedCountAfterLiveReply(200, 201, false), 201);
        QCOMPARE(threadExpectedCountAfterLiveReply(201, 201, false), 201);
        QCOMPARE(threadExpectedCountAfterLiveReply(201, 201, true), 201);
    }
};

QTEST_APPLESS_MAIN(ThreadLiveTimelineTest)

#include "ThreadLiveTimelineTest.moc"
