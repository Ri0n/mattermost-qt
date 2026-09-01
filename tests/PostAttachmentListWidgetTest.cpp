#include <QtTest>

#include <QListWidgetItem>
#include <QWidget>

#include "chat-area/post/attachments/PostAttachmentListWidget.h"

using namespace Mattermost;

namespace {

void settleEvents()
{
    for (int i = 0; i < 5; ++i) {
        QCoreApplication::processEvents();
    }
}

} // namespace

class PostAttachmentListWidgetTest : public QObject
{
    Q_OBJECT

private slots:
    void singleItemSizeHintIncludesOuterSpacing()
    {
        PostAttachmentListWidget list;
        list.setFrameShape(QFrame::NoFrame);
        list.setSpacing(10);

        auto* item = new QListWidgetItem(&list);
        item->setSizeHint(QSize(320, 500));

        QCOMPARE(list.sizeHint(), QSize(340, 520));
    }

    void multipleItemsIncludeOuterAndInterItemSpacing()
    {
        PostAttachmentListWidget list;
        list.setFrameShape(QFrame::NoFrame);
        list.setSpacing(10);

        auto* first = new QListWidgetItem(&list);
        first->setSizeHint(QSize(320, 500));
        auto* second = new QListWidgetItem(&list);
        second->setSizeHint(QSize(180, 100));

        QCOMPARE(list.sizeHint(), QSize(340, 630));
    }

    void sizeHintKeepsPreviewInsideViewport()
    {
        PostAttachmentListWidget list;
        list.setFrameShape(QFrame::NoFrame);
        list.setSpacing(10);
        list.setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list.setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

        auto* item = new QListWidgetItem(&list);
        item->setSizeHint(QSize(500, 300));

        auto* preview = new QWidget;
        preview->setFixedSize(500, 300);
        list.setItemWidget(item, preview);

        list.resize(list.sizeHint());
        list.show();
        settleEvents();

        const QRect viewportRect = list.viewport()->rect();
        const QRect itemRect = list.visualItemRect(item);

        QVERIFY2(viewportRect.contains(itemRect),
                 qPrintable(QStringLiteral(
                     "Attachment item must fit inside the viewport: viewport=%1,%2 %3x%4; item=%5,%6 %7x%8")
                     .arg(viewportRect.x()).arg(viewportRect.y())
                     .arg(viewportRect.width()).arg(viewportRect.height())
                     .arg(itemRect.x()).arg(itemRect.y())
                     .arg(itemRect.width()).arg(itemRect.height())));

        QCOMPARE(itemRect.topLeft(), QPoint(10, 10));
        QCOMPARE(itemRect.size(), QSize(500, 300));
        QCOMPARE(viewportRect.right() - itemRect.right(), 10);
        QCOMPARE(viewportRect.bottom() - itemRect.bottom(), 10);
    }
};

QTEST_MAIN(PostAttachmentListWidgetTest)

#include "PostAttachmentListWidgetTest.moc"
