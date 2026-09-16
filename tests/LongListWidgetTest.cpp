#include <QtTest>

#include <QEvent>
#include <QScrollBar>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QEnterEvent>
#endif

#include "widgets/LongListWidget.h"

namespace {

void settleEvents(int rounds = 8)
{
    for (int i = 0; i < rounds; ++i) {
        QCoreApplication::processEvents();
    }
}

void sendEnterEvent(QWidget* widget)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QPointF local(1.0, 1.0);
    const QPointF global = widget->mapToGlobal(QPoint(1, 1));
    QEnterEvent event(local, local, global);
#else
    QEvent event(QEvent::Enter);
#endif
    QCoreApplication::sendEvent(widget, &event);
}

class VariableRow : public QWidget
{
public:
    explicit VariableRow(int height, QWidget* parent = nullptr)
        : QWidget(parent)
        , hintHeight(height)
    {
    }

    QSize sizeHint() const override
    {
        return QSize(420, hintHeight);
    }

    QSize minimumSizeHint() const override
    {
        return sizeHint();
    }

    void setHintHeight(int height)
    {
        hintHeight = height;
        updateGeometry();
    }

private:
    int hintHeight = 1;
};

class TestLongListWidget : public Mattermost::LongListWidget
{
public:
    using Mattermost::LongListWidget::LongListWidget;

    void setSyntheticHeight(int index, int height)
    {
        syntheticHeights[index] = height;
        if (auto* row = static_cast<VariableRow*>(itemWidget(index))) {
            row->setHintHeight(height);
            // A real PostWidget emits dimensionsChanged after asynchronous
            // content reflow. Drive the same public invalidation path explicitly
            // rather than depending on platform-specific updateGeometry events.
            itemsChanged(index, index);
        }
    }

protected:
    QWidget* createItemWidget(int index) override
    {
        return new VariableRow(syntheticHeights.value(index, defaultItemHeight()));
    }

private:
    QHash<int, int> syntheticHeights;
};

} // namespace

class LongListWidgetTest : public QObject
{
    Q_OBJECT

private slots:
    void uniformGeometryMapsMiddleToMiddle()
    {
        TestLongListWidget list;
        list.resize(480, 400);
        list.setDefaultItemHeight(100);
        list.setItemCount(10000);
        list.show();
        settleEvents();

        QScrollBar* bar = list.verticalScrollBar();
        bar->setValue(bar->maximum() / 2);
        settleEvents();

        const int center = list.indexAtViewportPosition(list.viewport()->height() / 2);
        QVERIFY2(qAbs(center - 5000) <= 3,
                 "A uniform 10k list must map the middle of the scrollbar near logical item 5000");
    }

    void hoverStateFollowsLatestTopLevelItem()
    {
        TestLongListWidget list;
        list.resize(480, 180);
        list.setDefaultItemHeight(60);
        list.setItemCount(2);
        list.setRangeAvailable(0, 1);
        list.show();
        settleEvents();

        QWidget* first = list.itemWidget(0);
        QWidget* second = list.itemWidget(1);
        QVERIFY(first);
        QVERIFY(second);

        // Qt's offscreen QPA keeps a virtual cursor at a fixed screen position
        // and Qt 6 can therefore deliver a real Enter while show() settles.
        // Start the synthetic ordering check from a known empty hover state.
        list.setHoverHighlightEnabled(false);
        list.setHoverHighlightEnabled(true);

        QSignalSpy hoverChanges(&list, &Mattermost::LongListWidget::hoveredItemChanged);

        sendEnterEvent(first);
        QCOMPARE(hoverChanges.count(), 1);
        QCOMPARE(hoverChanges.at(0).at(0).toInt(), -1);
        QCOMPARE(hoverChanges.at(0).at(1).toInt(), 0);

        // Entering the next row may be observed before the stale Leave from
        // the previous one. The newest Enter must own hover state.
        sendEnterEvent(second);
        QCOMPARE(hoverChanges.count(), 2);
        QCOMPARE(hoverChanges.at(1).at(0).toInt(), 0);
        QCOMPARE(hoverChanges.at(1).at(1).toInt(), 1);

        QEvent leaveFirst(QEvent::Leave);
        QCoreApplication::sendEvent(first, &leaveFirst);
        QCOMPARE(hoverChanges.count(), 2);

        QEvent leaveSecond(QEvent::Leave);
        QCoreApplication::sendEvent(second, &leaveSecond);
        QCOMPARE(hoverChanges.count(), 3);
        QCOMPARE(hoverChanges.at(2).at(0).toInt(), 1);
        QCOMPARE(hoverChanges.at(2).at(1).toInt(), -1);
    }

    void missingItemsRequestContiguousViewportDemand()
    {
        TestLongListWidget list;
        list.resize(480, 360);
        list.setDefaultItemHeight(90);
        // requestBlockSize is a random-seek window hint, not transport paging.
        // Ordinary viewport demand must therefore be allowed to exceed it.
        list.setRequestBlockSize(3);
        list.setItemCount(1000);
        QSignalSpy requests(&list, &Mattermost::LongListWidget::rangeRequested);
        list.show();
        settleEvents();

        QCOMPARE(requests.count(), 1);
        const QList<QVariant> request = requests.takeFirst();
        const int requestedFirst = request.at(0).toInt();
        const int requestedLast = request.at(1).toInt();
        const auto visible = list.visibleRange();
        QVERIFY(visible.isValid());
        QVERIFY(requestedFirst <= visible.first);
        QVERIFY(requestedLast >= visible.last);
        QVERIFY2(requestedLast - requestedFirst + 1 > 3,
                 "Viewport demand must not be subdivided by the random-seek block size");
        QCOMPARE(list.materializedCount(), 0);
    }

    void prefetchUsesHalfScreenCapacity_data()
    {
        QTest::addColumn<int>("rowHeight");
        QTest::addColumn<int>("screenHeight");
        QTest::addColumn<int>("margin");
        QTest::newRow("short-messages") << 30 << 300 << 5;
        QTest::newRow("medium-messages") << 60 << 300 << 3;
        QTest::newRow("tall-message") << 600 << 300 << 1;
        QTest::newRow("larger-viewport") << 30 << 600 << 10;
    }

    void prefetchUsesHalfScreenCapacity()
    {
        QFETCH(int, rowHeight);
        QFETCH(int, screenHeight);
        QFETCH(int, margin);
        TestLongListWidget list;
        list.setFrameShape(QFrame::NoFrame);
        list.resize(480, screenHeight);
        list.setDefaultItemHeight(100); // deliberately differs from measured rows
        list.setItemCount(200);
        for (int i = 50; i < 200; ++i) list.setSyntheticHeight(i, rowHeight);
        list.setRangeAvailable(50, 199);
        list.show();
        list.scrollToIndex(50 + margin, Mattermost::LongListWidget::Alignment::Top);
        settleEvents(20);
        // Discard requests from initial measurement while the default height
        // still stood in for actual row heights.
        list.finishRangeRequest(0, 199);
        settleEvents();
        QSignalSpy requests(&list, &Mattermost::LongListWidget::rangeRequested);
        list.scrollToIndex(50 + margin, Mattermost::LongListWidget::Alignment::Top);
        settleEvents(12);
        QCOMPARE(requests.count(), 0);
        list.scrollToIndex(49 + margin, Mattermost::LongListWidget::Alignment::Top);
        settleEvents(12);
        bool requestedGap = false;
        for (const auto& request : requests) {
            if (request.at(0).toInt() <= 49 && request.at(1).toInt() >= 49) requestedGap = true;
        }
        QVERIFY2(requestedGap, "Half a screen of measured rows must be kept ahead of the viewport");
    }

    void materializationIsBoundedWithoutPlaceholderRows()
    {
        TestLongListWidget list;
        list.resize(480, 420);
        list.setDefaultItemHeight(70);
        list.setMaterializationLimit(80);
        list.setItemCount(10000);
        list.setRangeAvailable(0, 9999);
        list.show();
        settleEvents();

        QScrollBar* bar = list.verticalScrollBar();
        bar->setValue(bar->maximum() / 2);
        settleEvents();

        QVERIFY(list.materializedCount() > 0);
        QVERIFY2(list.materializedCount() <= 80,
                 "LongListWidget must never need one QWidget per logical item");
        const auto visible = list.visibleRange();
        const auto concrete = list.materializedRange();
        QVERIFY(concrete.contains(visible.first));
        QVERIFY(concrete.contains(visible.last));
    }

    void visitedRowsStayMaterializedUntilBudgetIsReached()
    {
        TestLongListWidget list;
        list.resize(480, 180);
        list.setDefaultItemHeight(60);
        list.setMaterializationLimit(200);
        list.setItemCount(21);
        list.setRangeAvailable(0, 20);
        list.show();
        settleEvents();

        for (int index = 0; index < 21; ++index) {
            list.scrollToIndex(index, Mattermost::LongListWidget::Alignment::Center);
            settleEvents(4);
        }

        QCOMPARE(list.materializedCount(), 21);
    }

    void tinyThumbDragInsideResidentWindowDoesNotSnapToEnd()
    {
        TestLongListWidget list;
        list.resize(480, 240);
        list.setDefaultItemHeight(60);
        list.setMaterializationLimit(200);
        list.setSeekDebounceMs(0);
        list.setItemCount(21);
        list.setRangeAvailable(0, 20);
        list.show();
        settleEvents();
        list.scrollToEnd();
        settleEvents();

        QScrollBar* bar = list.verticalScrollBar();
        QVERIFY(bar->maximum() > 1);
        const int draggedValue = bar->maximum() - 1;

        bar->setSliderDown(true);
        bar->setValue(draggedValue);
        QVERIFY(QMetaObject::invokeMethod(bar, "sliderMoved",
                                          Qt::DirectConnection,
                                          Q_ARG(int, draggedValue)));
        settleEvents(12);

        QCOMPARE(bar->value(), draggedValue);
        const auto visible = list.visibleRange();
        QVERIFY(visible.isValid());
        for (int index = visible.first; index <= visible.last; ++index) {
            QVERIFY2(list.itemWidget(index) != nullptr,
                     "A fully available small chat must not expose an empty viewport during thumb drag");
        }
        bar->setSliderDown(false);
    }

    void resolvedSeekNearBoundaryMaterializesWholeViewport_data()
    {
        QTest::addColumn<int>("target");
        QTest::addColumn<int>("first");
        QTest::addColumn<int>("last");
        QTest::newRow("oldest") << 5 << 0 << 31;
        QTest::newRow("newest") << 720 << 700 << 731;
    }

    void resolvedSeekNearBoundaryMaterializesWholeViewport()
    {
        QFETCH(int, target);
        QFETCH(int, first);
        QFETCH(int, last);
        TestLongListWidget list;
        list.resize(480, 360);
        list.setDefaultItemHeight(40);
        list.setPrefetchScreens(0);
        list.setSeekDebounceMs(0);
        list.setItemCount(732);
        for (int i = first; i <= last; ++i) list.setSyntheticHeight(i, 14);
        list.show();
        settleEvents();
        QSignalSpy requests(&list, &Mattermost::LongListWidget::rangeRequested);
        auto* bar = list.verticalScrollBar();
        const int value = bar->maximum() / 2;
        bar->setSliderDown(true);
        bar->setValue(value);
        QVERIFY(QMetaObject::invokeMethod(bar, "sliderMoved", Qt::DirectConnection, Q_ARG(int, value)));
        bar->setSliderDown(false);
        settleEvents();
        QVERIFY(!requests.isEmpty());
        const auto request = requests.last();
        const quint64 generation = request.at(3).toULongLong();
        QVERIFY(generation > 0);
        // A source resolves a cold seek at a boundary. The factory always
        // succeeds; there are no HTTP, eviction or delayed-availability races.
        list.setRangeAvailable(first, last);
        list.resolveSeekTarget(target, generation);
        list.finishRangeRequest(request.at(0).toInt(), request.at(1).toInt());
        settleEvents(20);
        const auto visible = list.visibleRange();
        QVERIFY(visible.isValid());
        for (int i = visible.first; i <= visible.last; ++i) {
            QVERIFY(list.isItemAvailable(i));
            QVERIFY2(list.itemWidget(i), qPrintable(QStringLiteral("Visible row %1 has no widget").arg(i)));
        }
    }

    void absoluteSliderMoveStartsSparseSeek()
    {
        TestLongListWidget list;
        list.resize(480, 320);
        list.setDefaultItemHeight(80);
        list.setRequestBlockSize(10);
        list.setSeekDebounceMs(0);
        list.setItemCount(10000);
        list.setRangeAvailable(0, 9);
        QSignalSpy requests(&list, &Mattermost::LongListWidget::rangeRequested);
        list.show();
        settleEvents(12);

        QScrollBar* bar = list.verticalScrollBar();
        QVERIFY(bar->maximum() > 0);
        bar->setValue(0);
        settleEvents(12);
        QVERIFY2(list.itemWidget(0) != nullptr,
                 "The starting viewport must be resident before the absolute sparse jump");
        requests.clear();

        QVERIFY(!bar->isSliderDown());

        // QScrollBar uses exactly this path for an absolute groove click:
        // setSliderPosition() with tracking enabled emits actionTriggered(SliderMove)
        // before the handle is marked down, but it does not emit sliderMoved().
        bar->setSliderPosition(bar->maximum() / 2);
        settleEvents(12);

        bool sawSeek = false;
        for (int i = 0; i < requests.count(); ++i) {
            const QList<QVariant> request = requests.at(i);
            // Ordinary scroll requests always use generation 0. A positive
            // generation is the stable cross-Qt observable for a random seek;
            // Qt5 QSignalSpy cannot decode the scoped RequestReason enum here.
            if (request.at(3).toULongLong() > 0) {
                sawSeek = true;
                break;
            }
        }
        QVERIFY2(sawSeek,
                 "An absolute jump from resident data into a sparse region must start a seek without a slider drag or model wake-up");
    }


    void newerSparseSeekOwnsOverlappingPendingRange()
    {
        TestLongListWidget list;
        list.resize(480, 320);
        list.setDefaultItemHeight(80);
        list.setRequestBlockSize(10);
        list.setSeekDebounceMs(0);
        list.setItemCount(1000);
        list.setRangeAvailable(0, 9);
        QSignalSpy requests(&list, &Mattermost::LongListWidget::rangeRequested);
        list.show();
        settleEvents(12);
        requests.clear();

        QScrollBar* bar = list.verticalScrollBar();
        const int firstValue = bar->maximum() / 2;
        bar->setSliderDown(true);
        bar->setValue(firstValue);
        QVERIFY(QMetaObject::invokeMethod(bar, "sliderMoved",
                                          Qt::DirectConnection,
                                          Q_ARG(int, firstValue)));
        bar->setSliderDown(false);
        settleEvents(12);

        quint64 firstGeneration = 0;
        for (int i = 0; i < requests.count(); ++i) {
            firstGeneration = std::max(firstGeneration,
                                       requests.at(i).at(3).toULongLong());
        }
        QVERIFY(firstGeneration > 0);

        // Leave generation 1 pending. The next target is only two rows away, so
        // its demand overlaps almost entirely with those old pending bits.
        requests.clear();
        const int secondValue = std::min(bar->maximum(), firstValue + 160);
        bar->setSliderDown(true);
        bar->setValue(secondValue);
        const int secondTarget = list.indexAtViewportPosition(
            list.viewport()->height() / 2);
        QVERIFY(QMetaObject::invokeMethod(bar, "sliderMoved",
                                          Qt::DirectConnection,
                                          Q_ARG(int, secondValue)));
        bar->setSliderDown(false);
        settleEvents(12);

        bool newGenerationOwnsTarget = false;
        for (int i = 0; i < requests.count(); ++i) {
            const QList<QVariant> request = requests.at(i);
            if (request.at(3).toULongLong() > firstGeneration
                && request.at(0).toInt() <= secondTarget
                && request.at(1).toInt() >= secondTarget) {
                newGenerationOwnsTarget = true;
                break;
            }
        }
        QVERIFY2(newGenerationOwnsTarget,
                 "A newer random seek must not inherit pending suppression from the previous generation");
    }

    void offTargetSeekProgressRetriggersDemand()
    {
        TestLongListWidget list;
        list.resize(480, 320);
        list.setDefaultItemHeight(80);
        list.setRequestBlockSize(10);
        list.setSeekDebounceMs(0);
        list.setItemCount(1000);
        list.setRangeAvailable(0, 9);
        QSignalSpy requests(&list, &Mattermost::LongListWidget::rangeRequested);
        list.show();
        settleEvents(12);
        requests.clear();

        QScrollBar* bar = list.verticalScrollBar();
        const int value = bar->maximum() / 2;
        bar->setSliderDown(true);
        bar->setValue(value);
        const int target = list.indexAtViewportPosition(list.viewport()->height() / 2);
        QVERIFY(QMetaObject::invokeMethod(bar, "sliderMoved",
                                          Qt::DirectConnection,
                                          Q_ARG(int, value)));
        bar->setSliderDown(false);
        settleEvents(12);
        QVERIFY(requests.count() > 0);

        const QList<QVariant> firstRequest = requests.takeFirst();
        const quint64 generation = firstRequest.at(3).toULongLong();
        QVERIFY(generation > 0);
        requests.clear();

        // Mirror the trace: the source learned an authoritative island away from
        // the requested viewport while this seek range remained pending.
        const int offTargetFirst = std::min(990, target + 30);
        list.setRangeAvailable(offTargetFirst, offTargetFirst + 3);
        settleEvents(4);
        requests.clear();

        list.finishRangeRequest(firstRequest.at(0).toInt(),
                                firstRequest.at(1).toInt());
        settleEvents(12);

        bool retriedTarget = false;
        for (int i = 0; i < requests.count(); ++i) {
            const QList<QVariant> request = requests.at(i);
            if (request.at(3).toULongLong() == generation
                && request.at(0).toInt() <= target
                && request.at(1).toInt() >= target) {
                retriedTarget = true;
                break;
            }
        }
        QVERIFY2(retriedTarget,
                 "Off-target source progress must wake the still-missing seek viewport after request completion");
    }

    void unresolvedSeekViewportDoesNotPollWithoutProgress()
    {
        TestLongListWidget list;
        list.resize(480, 320);
        list.setDefaultItemHeight(80);
        list.setRequestBlockSize(10);
        list.setSeekDebounceMs(0);
        list.setItemCount(1000);
        list.setRangeAvailable(0, 9);
        QSignalSpy requests(&list, &Mattermost::LongListWidget::rangeRequested);
        list.show();
        settleEvents(12);
        requests.clear();

        QScrollBar* bar = list.verticalScrollBar();
        const int value = bar->maximum() / 2;
        bar->setSliderDown(true);
        bar->setValue(value);
        QVERIFY(QMetaObject::invokeMethod(bar, "sliderMoved",
                                          Qt::DirectConnection,
                                          Q_ARG(int, value)));
        bar->setSliderDown(false);
        settleEvents(12);
        QVERIFY(requests.count() > 0);
        const QList<QVariant> request = requests.takeFirst();
        requests.clear();
        list.finishRangeRequest(request.at(0).toInt(), request.at(1).toInt());
        QTest::qWait(150);
        settleEvents(8);
        QCOMPARE(requests.count(), 0);
    }

    void ordinaryBlankViewportDoesNotPollWithoutProgress()
    {
        TestLongListWidget list;
        list.resize(480, 320);
        list.setDefaultItemHeight(80);
        list.setItemCount(1000);
        QSignalSpy requests(&list, &Mattermost::LongListWidget::rangeRequested);
        list.show();
        settleEvents(12);
        QVERIFY(requests.count() > 0);

        const QList<QVariant> request = requests.takeFirst();
        QCOMPARE(request.at(3).toULongLong(), quint64(0));
        requests.clear();
        list.finishRangeRequest(request.at(0).toInt(), request.at(1).toInt());
        QTest::qWait(150);
        settleEvents(8);
        QCOMPARE(requests.count(), 0);
    }

    void delayedRowGrowthKeepsStickyBottom()
    {
        TestLongListWidget list;
        list.resize(480, 320);
        list.setDefaultItemHeight(60);
        list.setItemCount(200);
        list.setRangeAvailable(0, 199);
        list.show();
        settleEvents();
        list.scrollToEnd();
        settleEvents();

        QCOMPARE(list.verticalScrollBar()->value(), list.verticalScrollBar()->maximum());
        QVERIFY(list.visibleRange().contains(199));

        const int growIndex = std::max(0, list.visibleRange().first);
        list.setSyntheticHeight(growIndex, 260);
        settleEvents(12);

        QCOMPARE(list.verticalScrollBar()->value(), list.verticalScrollBar()->maximum());
        QVERIFY2(list.visibleRange().contains(199),
                 "Late sizeHint changes must not detach a sticky-bottom viewport from the end");
    }

    void itemCountGrowthKeepsStickyBottom()
    {
        TestLongListWidget list;
        list.resize(480, 320);
        list.setDefaultItemHeight(60);
        list.setItemCount(200);
        list.setRangeAvailable(0, 199);
        list.show();
        settleEvents();
        list.scrollToEnd();
        settleEvents();

        QCOMPARE(list.verticalScrollBar()->value(), list.verticalScrollBar()->maximum());
        QVERIFY(list.visibleRange().contains(199));

        list.setItemCount(201);
        list.setRangeAvailable(200, 200);
        settleEvents(12);

        QCOMPARE(list.verticalScrollBar()->value(), list.verticalScrollBar()->maximum());
        QVERIFY2(list.visibleRange().contains(200),
                 "Appending a logical item must keep an already sticky viewport at the new end");
    }

    void oversizedItemCountGrowthKeepsTailLowerEdgeVisible()
    {
        TestLongListWidget list;
        list.resize(480, 320);
        list.setDefaultItemHeight(60);
        list.setItemCount(200);
        list.setRangeAvailable(0, 199);
        list.show();
        settleEvents();
        list.scrollToEnd();
        settleEvents();

        list.setSyntheticHeight(200, 720);
        list.setItemCount(201);
        list.setRangeAvailable(200, 200);
        settleEvents(16);

        QWidget* tail = list.itemWidget(200);
        QVERIFY(tail != nullptr);
        QVERIFY(tail->height() > list.viewport()->height());
        QCOMPARE(list.verticalScrollBar()->value(), list.verticalScrollBar()->maximum());
        QVERIFY2(qAbs(tail->y() + tail->height() - list.viewport()->height()) <= 2,
                 "An oversized appended tail must keep its lower edge inside a sticky-bottom viewport");
    }

    void prependShiftsLogicalAnchorWithoutMovingContent()
    {
        TestLongListWidget list;
        list.resize(480, 320);
        list.setDefaultItemHeight(64);
        list.setItemCount(200);
        list.setRangeAvailable(0, 199);
        list.show();
        settleEvents();

        list.scrollToIndex(100, Mattermost::LongListWidget::Alignment::Top);
        settleEvents();
        const int before = list.indexAtViewportPosition(2);
        QCOMPARE(before, 100);

        list.insertItems(0, 10);
        list.setRangeAvailable(0, 9);
        settleEvents(12);

        const int after = list.indexAtViewportPosition(2);
        QCOMPARE(after, before + 10);
        QVERIFY2(list.materializedRange().contains(after),
                 "Prepending real items must shift the materialized logical window with its anchor");
    }

    void prependKeepsStickyBottomOnSameNewestItem()
    {
        TestLongListWidget list;
        list.resize(480, 320);
        list.setDefaultItemHeight(60);
        list.setItemCount(200);
        list.setRangeAvailable(0, 199);
        list.show();
        settleEvents();
        list.scrollToEnd();
        settleEvents();

        list.insertItems(0, 10);
        list.setRangeAvailable(0, 9);
        settleEvents(12);

        QCOMPARE(list.verticalScrollBar()->value(), list.verticalScrollBar()->maximum());
        QVERIFY2(list.visibleRange().contains(209),
                 "Discovering older items must not detach a sticky viewport from the same newest item");
    }

    void viewportLockKeepsSameYAcrossReflow()
    {
        TestLongListWidget list;
        list.resize(480, 400);
        list.setDefaultItemHeight(60);
        list.setItemCount(300);
        list.setRangeAvailable(0, 299);
        list.show();
        settleEvents();

        QVERIFY(list.lockViewportToItem(150,
                                        Mattermost::LongListWidget::Alignment::Center,
                                        0));
        settleEvents(12);
        QWidget* locked = list.itemWidget(150);
        QVERIFY(locked != nullptr);
        const int yBefore = locked->y();

        QVERIFY2(list.materializedRange().contains(149),
                 "The row immediately above a centered lock should be materialized for this test");
        list.setSyntheticHeight(149, 260);
        settleEvents(16);

        locked = list.itemWidget(150);
        QVERIFY(locked != nullptr);
        QVERIFY2(qAbs(locked->y() - yBefore) <= 2,
                 "Reflow above a locked item must keep its viewport Y stable");
    }

    void navigationLockRecentersWhenTargetHeightChanges()
    {
        TestLongListWidget list;
        list.resize(480, 400);
        list.setDefaultItemHeight(60);
        list.setItemCount(300);
        list.setRangeAvailable(0, 299);
        list.show();
        settleEvents();

        QVERIFY(list.lockViewportToItem(150,
                                        Mattermost::LongListWidget::Alignment::Center,
                                        0));
        settleEvents(12);
        QWidget* target = list.itemWidget(150);
        QVERIFY(target != nullptr);

        list.setSyntheticHeight(150, 260);
        settleEvents(16);

        target = list.itemWidget(150);
        QVERIFY(target != nullptr);
        const int expectedY = (list.viewport()->height() - target->height()) / 2;
        QVERIFY2(qAbs(target->y() - expectedY) <= 2,
                 "A navigation target that grows but still fits must be re-centred using its real height");

        list.setSyntheticHeight(150, 520);
        settleEvents(16);
        target = list.itemWidget(150);
        QVERIFY(target != nullptr);
        QVERIFY2(qAbs(target->y()) <= 2,
                 "A navigation target taller than the viewport must be aligned to the viewport top");
    }

    void oversizedNavigationTargetTopAlignsAfterMaterialization()
    {
        TestLongListWidget list;
        list.resize(480, 400);
        list.setDefaultItemHeight(60);
        list.setItemCount(300);
        list.setRangeAvailable(0, 299);
        list.setSyntheticHeight(150, 520);
        list.show();
        settleEvents();

        QVERIFY(list.lockViewportToItem(150,
                                        Mattermost::LongListWidget::Alignment::Center,
                                        0));
        settleEvents(16);

        QWidget* target = list.itemWidget(150);
        QVERIFY(target != nullptr);
        QVERIFY(target->height() > list.viewport()->height());
        QVERIFY2(qAbs(target->y()) <= 2,
                 "An oversized semantic target must expose its beginning instead of clipping both ends");
    }

    void centeredLastItemUsesNewestEdgeWhenItFits()
    {
        TestLongListWidget list;
        list.resize(480, 400);
        list.setDefaultItemHeight(60);
        list.setItemCount(100);
        list.setRangeAvailable(0, 99);
        list.show();
        settleEvents();

        QVERIFY(list.lockViewportToItem(99,
                                        Mattermost::LongListWidget::Alignment::Center,
                                        0));
        settleEvents(16);

        QWidget* target = list.itemWidget(99);
        QVERIFY(target != nullptr);
        QCOMPARE(list.verticalScrollBar()->value(), list.verticalScrollBar()->maximum());
        QVERIFY2(qAbs(target->y() + target->height() - list.viewport()->height()) <= 2,
                 "A fitting last target must clamp to the newest edge rather than leave blank space below");
    }

    void unavailableMeasuredItemResetsEstimateWithoutMovingLock()
    {
        TestLongListWidget list;
        list.resize(480, 400);
        list.setDefaultItemHeight(60);
        list.setItemCount(300);
        list.setRangeAvailable(0, 299);
        list.show();
        settleEvents();

        QVERIFY(list.lockViewportToItem(150,
                                        Mattermost::LongListWidget::Alignment::Center,
                                        0));
        settleEvents(12);
        QVERIFY(list.itemWidget(149) != nullptr);

        list.setSyntheticHeight(149, 260);
        settleEvents(16);
        QWidget* locked = list.itemWidget(150);
        QVERIFY(locked != nullptr);
        const int yBefore = locked->y();
        const qint64 heightBefore = list.contentHeight();

        list.setRangeAvailable(149, 149, false);
        settleEvents(16);

        locked = list.itemWidget(150);
        QVERIFY(locked != nullptr);
        QCOMPARE(list.contentHeight(), heightBefore - 200);
        QVERIFY2(qAbs(locked->y() - yBefore) <= 2,
                 "Dropping stale provisional geometry must preserve the active viewport lock");
    }

    void bodyAvailabilityDropRerequestsSameLogicalIdentity()
    {
        TestLongListWidget list;
        list.resize(480, 320);
        list.setDefaultItemHeight(60);
        list.setRequestBlockSize(10);
        list.setItemCount(100);
        list.setRangeAvailable(0, 99);
        list.show();
        settleEvents();

        list.scrollToIndex(55, Mattermost::LongListWidget::Alignment::Center);
        settleEvents(12);
        QVERIFY2(list.itemWidget(55) != nullptr,
                 "The target must be materialized before simulating body eviction");

        QSignalSpy requests(&list, &Mattermost::LongListWidget::rangeRequested);
        list.setRangeAvailable(55, 55, false);
        settleEvents(12);

        QVERIFY(!list.isItemAvailable(55));
        QCOMPARE(list.itemWidget(55), nullptr);

        bool requestedIdentity = false;
        for (int i = 0; i < requests.count(); ++i) {
            const QList<QVariant> request = requests.at(i);
            if (request.at(0).toInt() == 55 && request.at(1).toInt() == 55) {
                requestedIdentity = true;
                break;
            }
        }
        QVERIFY2(requestedIdentity,
                 "Dropping only a resident body must re-request that logical identity without artificial block expansion");

        // Rematerialization restores body availability only. No item-count or
        // structural mutation is needed for the same semantic source identity.
        list.setRangeAvailable(55, 55, true);
        settleEvents(12);
        QVERIFY(list.isItemAvailable(55));
        QVERIFY2(list.itemWidget(55) != nullptr,
                 "Restoring the body must materialize the same logical item again");
    }

    void viewportLockScalesRelativeYAcrossResize()
    {
        TestLongListWidget list;
        list.resize(480, 500);
        list.setDefaultItemHeight(60);
        list.setItemCount(300);
        list.setRangeAvailable(0, 299);
        list.show();
        settleEvents();

        QVERIFY(list.lockViewportToItem(150,
                                        Mattermost::LongListWidget::Alignment::Center,
                                        0));
        settleEvents(12);
        QWidget* locked = list.itemWidget(150);
        QVERIFY(locked != nullptr);
        const int yBefore = locked->y();
        const int viewportBefore = list.viewport()->height();
        QVERIFY(viewportBefore > 0);

        list.resize(480, 260);
        settleEvents(16);

        locked = list.itemWidget(150);
        QVERIFY(locked != nullptr);
        const int viewportAfter = list.viewport()->height();
        const int expectedY = qRound(static_cast<qreal>(yBefore)
            * static_cast<qreal>(viewportAfter) / static_cast<qreal>(viewportBefore));
        QVERIFY2(qAbs(locked->y() - expectedY) <= 2,
                 "A locked item's Y must scale with viewport height across resize");

        list.resize(480, 500);
        settleEvents(16);
        locked = list.itemWidget(150);
        QVERIFY(locked != nullptr);
        QVERIFY2(qAbs(locked->y() - yBefore) <= 2,
                 "Expanding the viewport again must restore the locked item's original relative Y");
    }

    void viewportLockRemapKeepsScreenPosition()
    {
        TestLongListWidget list;
        list.resize(480, 400);
        list.setDefaultItemHeight(60);
        list.setItemCount(300);
        list.setRangeAvailable(0, 299);
        list.show();
        settleEvents();

        QVERIFY(list.lockViewportToItem(120,
                                        Mattermost::LongListWidget::Alignment::Center,
                                        0));
        settleEvents(12);
        QWidget* oldTarget = list.itemWidget(120);
        QVERIFY(oldTarget != nullptr);
        const int yBefore = oldTarget->y();

        QVERIFY(list.remapViewportLockedItem(170));
        settleEvents(16);

        QWidget* newTarget = list.itemWidget(170);
        QVERIFY(newTarget != nullptr);
        QVERIFY2(qAbs(newTarget->y() - yBefore) <= 2,
                 "Authoritative logical remap must not recenter or otherwise move the locked target");
    }

    void lateGeometryCannotLeaveViewportWithoutMaterializedItems()
    {
        TestLongListWidget list;
        list.resize(520, 380);
        list.setDefaultItemHeight(96);
        list.setItemCount(10000);
        list.setRangeAvailable(9970, 9999);
        list.show();
        list.scrollToEnd();
        settleEvents();

        QVERIFY(list.materializedCount() > 0);
        QVERIFY(list.visibleRange().contains(9999));

        const QVector<int> indices = list.materializedIndices();
        for (int i = 0; i < indices.size(); ++i) {
            list.setSyntheticHeight(indices.at(i), 55 + (i % 5) * 37);
        }
        settleEvents(16);

        const auto visible = list.visibleRange();
        const auto concrete = list.materializedRange();
        QVERIFY2(visible.isValid(), "The viewport must remain mapped to logical items after reflow");
        QVERIFY2(concrete.isValid(), "Reflow must not evict the whole visible materialized window");
        QVERIFY(concrete.contains(visible.first));
        QVERIFY(concrete.contains(visible.last));
        QVERIFY(visible.contains(9999));
    }
};

QTEST_MAIN(LongListWidgetTest)

#include "LongListWidgetTest.moc"