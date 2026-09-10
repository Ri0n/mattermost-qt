#include <QtTest>

#include <QScrollBar>

#include "widgets/LongListWidget.h"

namespace {

void settleEvents(int rounds = 8)
{
    for (int i = 0; i < rounds; ++i) {
        QCoreApplication::processEvents();
    }
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

    void missingItemsRequestWholeBlocksWithoutGapWidgets()
    {
        TestLongListWidget list;
        list.resize(480, 360);
        list.setDefaultItemHeight(90);
        list.setRequestBlockSize(10);
        list.setItemCount(1000);
        QSignalSpy requests(&list, &Mattermost::LongListWidget::rangeRequested);
        list.show();
        settleEvents();

        QVERIFY2(requests.count() > 0,
                 "An unavailable visible range must request data");
        const QList<QVariant> first = requests.takeFirst();
        const int requestedFirst = first.at(0).toInt();
        const int requestedLast = first.at(1).toInt();
        QCOMPARE(requestedFirst % 10, 0);
        QCOMPARE(requestedLast - requestedFirst + 1, 10);
        QCOMPARE(list.materializedCount(), 0);
    }

    void requestsGapBeforeFewerThanFiveItemsRemain()
    {
        TestLongListWidget list;
        list.resize(480, 120);
        list.setDefaultItemHeight(100);
        list.setRequestBlockSize(10);
        list.setPrefetchScreens(0);
        list.setItemCount(100);
        list.setRangeAvailable(50, 99);
        QSignalSpy requests(&list, &Mattermost::LongListWidget::rangeRequested);
        list.show();
        settleEvents();
        requests.clear();

        // Exactly five known logical rows remain before the gap: 50..54.
        // The hard margin may include index 50, but it must not yet reach 49.
        list.scrollToIndex(55, Mattermost::LongListWidget::Alignment::Top);
        settleEvents(12);
        QCOMPARE(requests.count(), 0);

        // Moving one item upward leaves only four known rows (50..53) before
        // the gap. The desired range must now include index 49 and therefore
        // request its whole 10-item block before the viewport reaches the gap.
        list.scrollToIndex(54, Mattermost::LongListWidget::Alignment::Top);
        settleEvents(12);
        QVERIFY2(requests.count() > 0,
                 "A gap must be requested before fewer than five known items remain");

        bool requestedPreviousBlock = false;
        for (int i = 0; i < requests.count(); ++i) {
            const QList<QVariant> request = requests.at(i);
            if (request.at(0).toInt() == 40 && request.at(1).toInt() == 49) {
                requestedPreviousBlock = true;
                break;
            }
        }
        QVERIFY2(requestedPreviousBlock,
                 "The five-item logical prefetch margin must request block 40..49");
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
        QCOMPARE(tail->y() + tail->height(), list.viewport()->height());
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
        list.setItemCount(300);
        list.setRangeAvailable(0, 299);
        list.show();
        settleEvents();

        QVERIFY(list.lockViewportToItem(299,
                                        Mattermost::LongListWidget::Alignment::Center,
                                        0));
        settleEvents(12);

        QWidget* target = list.itemWidget(299);
        QVERIFY(target != nullptr);
        QCOMPARE(list.verticalScrollBar()->value(), list.verticalScrollBar()->maximum());
        QVERIFY2(target->y() + target->height() <= list.viewport()->height(),
                 "The newest fitting row must not be pushed below the viewport while centered");
    }

    void centeredLastOversizedItemKeepsBeginningVisible()
    {
        TestLongListWidget list;
        list.resize(480, 400);
        list.setDefaultItemHeight(60);
        list.setItemCount(300);
        list.setRangeAvailable(0, 299);
        list.setSyntheticHeight(299, 620);
        list.show();
        settleEvents();

        QVERIFY(list.lockViewportToItem(299,
                                        Mattermost::LongListWidget::Alignment::Center,
                                        0));
        settleEvents(16);

        QWidget* target = list.itemWidget(299);
        QVERIFY(target != nullptr);
        QVERIFY(target->height() > list.viewport()->height());
        QVERIFY2(qAbs(target->y()) <= 2,
                 "Semantic navigation to an oversized newest item must start at its beginning");
    }

    void viewportLockSurvivesPrependByIdentityIndexShift()
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
        const int yBefore = target->y();

        list.insertItems(0, 10);
        list.setRangeAvailable(0, 9);
        settleEvents(12);

        target = list.itemWidget(160);
        QVERIFY(target != nullptr);
        QVERIFY2(qAbs(target->y() - yBefore) <= 2,
                 "Prepending older logical items must shift the viewport lock with its target identity");
    }

    void removedLockedItemReleasesViewportLock()
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
        QVERIFY(list.hasViewportLock());

        list.removeItems(150, 1);
        settleEvents(8);
        QVERIFY(!list.hasViewportLock());
    }

    void removingBeforeLockedItemShiftsLockWithoutMovingTarget()
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
        const int yBefore = target->y();

        list.removeItems(0, 10);
        settleEvents(12);

        target = list.itemWidget(140);
        QVERIFY(target != nullptr);
        QVERIFY(list.hasViewportLock());
        QVERIFY2(qAbs(target->y() - yBefore) <= 2,
                 "Removing older logical items must shift the viewport lock with its target identity");
    }

    void ensureVisibleDoesNotReleaseSameItemViewportLock()
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
        const int yBefore = target->y();

        list.scrollToIndex(150, Mattermost::LongListWidget::Alignment::EnsureVisible);
        settleEvents(12);

        target = list.itemWidget(150);
        QVERIFY(target != nullptr);
        QVERIFY(list.hasViewportLock());
        QCOMPARE(target->y(), yBefore);
    }

    void userScrollReleasesViewportLock()
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
        QVERIFY(list.hasViewportLock());

        QWheelEvent wheel(QPointF(20, 20), QPointF(20, 20), QPoint(), QPoint(0, -120),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(list.viewport(), &wheel);
        settleEvents(8);
        QVERIFY(!list.hasViewportLock());
    }

    void programmaticScrollDoesNotEmitUserViewportChanged()
    {
        TestLongListWidget list;
        list.resize(480, 400);
        list.setDefaultItemHeight(60);
        list.setItemCount(300);
        list.setRangeAvailable(0, 299);
        list.show();
        settleEvents();

        QSignalSpy userChanges(&list, &Mattermost::LongListWidget::userViewportChanged);
        list.scrollToIndex(150, Mattermost::LongListWidget::Alignment::Center);
        settleEvents(12);
        QCOMPARE(userChanges.count(), 0);
    }

    void wheelScrollEmitsUserViewportChanged()
    {
        TestLongListWidget list;
        list.resize(480, 400);
        list.setDefaultItemHeight(60);
        list.setItemCount(300);
        list.setRangeAvailable(0, 299);
        list.show();
        settleEvents();

        QSignalSpy userChanges(&list, &Mattermost::LongListWidget::userViewportChanged);
        QWheelEvent wheel(QPointF(20, 20), QPointF(20, 20), QPoint(), QPoint(0, -120),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(list.viewport(), &wheel);
        settleEvents(8);
        QCOMPARE(userChanges.count(), 1);
    }
};

QTEST_MAIN(LongListWidgetTest)
#include "LongListWidgetTest.moc"
