#include <QtTest>

#include "widgets/LongListWidget.h"

namespace {

void settleEvents(int rounds = 12)
{
    for (int i = 0; i < rounds; ++i) {
        QCoreApplication::processEvents();
    }
}

class FixedRow final : public QWidget
{
public:
    using QWidget::QWidget;

    QSize sizeHint() const override { return QSize(420, 64); }
    QSize minimumSizeHint() const override { return sizeHint(); }
};

class TestList final : public Mattermost::LongListWidget
{
public:
    using Mattermost::LongListWidget::LongListWidget;

protected:
    QWidget* createItemWidget(int) override
    {
        return new FixedRow;
    }
};

} // namespace

class LongListWidgetRemovalTest : public QObject
{
    Q_OBJECT

private slots:
    void removalBeforeViewportPreservesConcreteRow()
    {
        TestList list;
        list.resize(480, 320);
        list.setDefaultItemHeight(64);
        list.setItemCount(200);
        list.setRangeAvailable(0, 199);
        list.show();
        settleEvents();

        list.scrollToIndex(100, Mattermost::LongListWidget::Alignment::Top);
        settleEvents();
        QWidget* row = list.itemWidget(100);
        QVERIFY(row != nullptr);
        const int yBefore = row->y();

        list.removeItems(0, 10);
        settleEvents();

        QCOMPARE(list.itemCount(), 190);
        QCOMPARE(list.itemWidget(90), row);
        QVERIFY2(qAbs(row->y() - yBefore) <= 2,
                 "Removing older logical rows must shift the same concrete widget without moving it on screen");
    }

    void removingCurrentRowPromotesFollowingIdentity()
    {
        TestList list;
        list.resize(480, 320);
        list.setDefaultItemHeight(64);
        list.setItemCount(200);
        list.setRangeAvailable(0, 199);
        list.show();
        settleEvents();

        list.scrollToIndex(50, Mattermost::LongListWidget::Alignment::Top);
        settleEvents();
        QWidget* following = list.itemWidget(51);
        QVERIFY(following != nullptr);

        list.removeItems(50, 1);
        settleEvents();

        QCOMPARE(list.itemCount(), 199);
        QCOMPARE(list.itemWidget(50), following);
    }

    void removingPhantomOldestPrefixEliminatesBlankTop()
    {
        TestList list;
        list.resize(480, 320);
        list.setDefaultItemHeight(96);
        list.setItemCount(1070);
        list.setRangeAvailable(3, 9);
        list.show();
        list.scrollToIndex(0, Mattermost::LongListWidget::Alignment::Top);
        settleEvents();

        QVERIFY(list.itemWidget(3) != nullptr);
        QVERIFY(list.itemWidget(0) == nullptr);

        list.removeItems(0, 3);
        settleEvents();

        QCOMPARE(list.itemCount(), 1067);
        QVERIFY(list.itemWidget(0) != nullptr);
        QVERIFY2(qAbs(list.itemWidget(0)->y()) <= 2,
                 "Once the server proves leading logical slots are phantom, the oldest real post must occupy the top");
    }
};

QTEST_MAIN(LongListWidgetRemovalTest)

#include "LongListWidgetRemovalTest.moc"
