#include <QtTest>

#include <QListWidgetItem>

#include "chat-area/post/attachments/PostAttachmentListWidget.h"

using namespace Mattermost;

class PostAttachmentListWidgetTest : public QObject
{
    Q_OBJECT

private slots:
    void singleItemSizeHintMatchesItem()
    {
        PostAttachmentListWidget list;
        list.setFrameShape(QFrame::NoFrame);
        list.setSpacing(10);

        auto* item = new QListWidgetItem(&list);
        item->setSizeHint(QSize(320, 500));

        QCOMPARE(list.sizeHint(), QSize(320, 500));
    }

    void multipleItemsAddOnlyConfiguredSpacing()
    {
        PostAttachmentListWidget list;
        list.setFrameShape(QFrame::NoFrame);
        list.setSpacing(10);

        auto* first = new QListWidgetItem(&list);
        first->setSizeHint(QSize(320, 500));
        auto* second = new QListWidgetItem(&list);
        second->setSizeHint(QSize(180, 100));

        QCOMPARE(list.sizeHint(), QSize(320, 610));
    }
};

QTEST_MAIN(PostAttachmentListWidgetTest)

#include "PostAttachmentListWidgetTest.moc"
