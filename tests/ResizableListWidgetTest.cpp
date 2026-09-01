#include <QtTest>

#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "chat-area/ResizableListWidget.h"

namespace {

void settleEvents()
{
    for (int i = 0; i < 5; ++i) {
        QCoreApplication::processEvents();
    }
}

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
};

QTEST_MAIN(ResizableListWidgetTest)

#include "ResizableListWidgetTest.moc"
