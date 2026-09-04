#include <QtTest>

#include <QLabel>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>

#include "chat-area/ResizableListWidget.h"

namespace {

void settleEvents()
{
    for (int i = 0; i < 5; ++i) {
        QCoreApplication::processEvents();
    }
}

QListWidgetItem* bottomVisibleItem(QListWidget& list)
{
    const QRect viewportRect = list.viewport()->rect();
    for (int row = list.count() - 1; row >= 0; --row) {
        QListWidgetItem* item = list.item(row);
        if (list.visualItemRect(item).intersects(viewportRect)) {
            return item;
        }
    }
    return nullptr;
}

class VariableHeightRow : public QWidget
{
public:
    explicit VariableHeightRow(int height, QWidget* parent = nullptr)
        : QWidget(parent)
        , hintHeight(height)
    {
    }

    QSize sizeHint() const override
    {
        return QSize(320, hintHeight);
    }

    QSize minimumSizeHint() const override
    {
        return sizeHint();
    }

    void setHintHeight(int height)
    {
        hintHeight = height;
    }

private:
    int hintHeight;
};

class ExternalAnchorResizableList : public ResizableListWidget
{
public:
    using ResizableListWidget::ResizableListWidget;

    void schedulePreservingResize(QListWidgetItem* item, QWidget* widget)
    {
        scheduleItemResize(item, widget, true);
    }
};

} // namespace

class ResizableListWidgetTest : public QObject
{
    Q_OBJECT

private slots:
    void compositeRowTracksAsynchronousContentGrowth()
    {
        ResizableListWidget list;
        list.resize(520, 500);

        auto* row = new QWidget;
        auto* layout = new QVBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);

        auto* text = new QLabel(QStringLiteral("first line\nsecond line\nthird line"), row);
        text->setFixedHeight(60);
        layout->addWidget(text);

        auto* attachment = new QWidget(row);
        attachment->setFixedSize(320, 40);
        layout->addWidget(attachment, 0, Qt::AlignLeft);

        auto* threadButton = new QPushButton(QStringLiteral("Open Thread"), row);
        threadButton->setFixedHeight(32);
        layout->addWidget(threadButton);

        auto* item = new QListWidgetItem;
        item->setSizeHint(QSize(500, 20));
        list.addItem(item);
        list.setItemWidget(item, row);

        list.show();
        settleEvents();

        const int initialHeight = item->sizeHint().height();
        QVERIFY2(initialHeight >= row->sizeHint().height(),
                 "The list row must initially fit all composite post contents");

        attachment->setFixedHeight(280);
        row->updateGeometry();
        settleEvents();

        const int grownHeight = item->sizeHint().height();
        QVERIFY2(grownHeight >= row->sizeHint().height(),
                 "A delayed attachment resize must grow the QListWidgetItem row");
        QVERIFY2(grownHeight >= initialHeight + 200,
                 "The row must not remain at its old height after attachment growth");
        QVERIFY2(list.visualItemRect(item).height() >= row->sizeHint().height(),
                 "The actual painted row must fit text, attachment and thread button");
    }

    void contentGrowthKeepsBottomVisibleRowAnchored()
    {
        ResizableListWidget list;
        list.resize(360, 180);

        QList<QWidget*> rows;
        for (int i = 0; i < 10; ++i) {
            auto* row = new QWidget;
            auto* layout = new QVBoxLayout(row);
            layout->setContentsMargins(0, 0, 0, 0);
            auto* content = new QLabel(QStringLiteral("row %1").arg(i), row);
            content->setFixedHeight(48);
            layout->addWidget(content);

            auto* item = new QListWidgetItem;
            list.addItem(item);
            list.setItemWidget(item, row);
            rows.push_back(row);
        }

        list.show();
        settleEvents();
        list.verticalScrollBar()->setValue(150);
        settleEvents();

        QListWidgetItem* anchor = bottomVisibleItem(list);
        QVERIFY(anchor);
        const int anchorRow = list.row(anchor);
        const int bottomOffsetBefore = list.viewport()->rect().bottom() - list.visualItemRect(anchor).bottom();

        // Simulate an image finishing above the current viewport. Without
        // visual anchoring this pushes all visible messages down and changes
        // which message is at the bottom of the chat.
        rows.at(0)->setMinimumHeight(220);
        rows.at(0)->updateGeometry();
        settleEvents();

        QListWidgetItem* anchorAfter = list.item(anchorRow);
        QVERIFY(anchorAfter);
        const int bottomOffsetAfter = list.viewport()->rect().bottom() - list.visualItemRect(anchorAfter).bottom();
        QCOMPARE(bottomVisibleItem(list), anchorAfter);
        QVERIFY2(qAbs(bottomOffsetAfter - bottomOffsetBefore) <= 2,
                 "Delayed row growth must preserve the bottom-most visible row and its viewport offset");
    }

    void externalViewportOwnerIsNotOverriddenByDeferredBaseRestore()
    {
        ExternalAnchorResizableList list;
        list.setProperty("_mattermostExternalViewportAnchor", true);
        list.resize(360, 180);

        QList<VariableHeightRow*> rows;
        QList<QListWidgetItem*> items;
        for (int i = 0; i < 20; ++i) {
            auto* row = new VariableHeightRow(48);
            auto* item = new QListWidgetItem;
            list.addItem(item);
            list.setItemWidget(item, row);
            rows.push_back(row);
            items.push_back(item);
        }

        list.show();
        settleEvents();
        list.verticalScrollBar()->setValue(120);
        settleEvents();

        rows.first()->setHintHeight(240);
        list.schedulePreservingResize(items.first(), rows.first());

        int userSelectedValue = -1;
        // The resize callback runs first and, in the old implementation, queues
        // a second delayed base-anchor restore. This callback models a later user
        // wheel/seek intent in between those two stages. An externally anchored
        // sparse list must retain this newer position instead of being pulled
        // back by the stale ResizableListWidget anchor.
        QTimer::singleShot(0, &list, [&list, &userSelectedValue] {
            userSelectedValue = std::min(260, list.verticalScrollBar()->maximum());
            list.verticalScrollBar()->setValue(userSelectedValue);
        });

        settleEvents();
        QVERIFY(userSelectedValue >= 0);
        QCOMPARE(list.verticalScrollBar()->value(), userSelectedValue);
    }

    void removedRowInvalidatesPendingResize()
    {
        ResizableListWidget list;
        list.resize(520, 300);

        auto* row = new QWidget;
        auto* layout = new QVBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(new QLabel(QStringLiteral("queued resize"), row));

        auto* item = new QListWidgetItem;
        list.addItem(item);
        list.setItemWidget(item, row);

        // setItemWidget() schedules a zero-timeout resize. Remove the model row
        // before that callback can run. QPersistentModelIndex must invalidate it
        // so the callback never dereferences the deleted QListWidgetItem.
        delete list.takeItem(0);
        settleEvents();

        QCOMPARE(list.count(), 0);
    }
};

QTEST_MAIN(ResizableListWidgetTest)

#include "ResizableListWidgetTest.moc"
