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
