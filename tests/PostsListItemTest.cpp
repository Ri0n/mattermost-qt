#include <QtTest>

#include "chat-area/PostsListWidget.h"

using namespace Mattermost;

class PostsListItemTest : public QObject
{
    Q_OBJECT

private slots:
    void classifiesListItems()
    {
        QVERIFY(!PostsListWidget::isPostItem(nullptr));

        QListWidgetItem item;
        QVERIFY(!PostsListWidget::isPostItem(&item));

        item.setData(Qt::UserRole, ItemType::separator);
        QVERIFY(!PostsListWidget::isPostItem(&item));

        item.setData(Qt::UserRole, ItemType::post);
        QVERIFY(PostsListWidget::isPostItem(&item));
    }
};

QTEST_APPLESS_MAIN(PostsListItemTest)

#include "PostsListItemTest.moc"
