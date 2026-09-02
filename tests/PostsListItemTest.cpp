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
        QVERIFY(!PostsListWidget::isGapItem(nullptr));

        QListWidgetItem item;
        QVERIFY(!PostsListWidget::isPostItem(&item));
        QVERIFY(!PostsListWidget::isGapItem(&item));

        item.setData(Qt::UserRole, ItemType::separator);
        QVERIFY(!PostsListWidget::isPostItem(&item));
        QVERIFY(!PostsListWidget::isGapItem(&item));

        item.setData(Qt::UserRole, ItemType::post);
        QVERIFY(PostsListWidget::isPostItem(&item));
        QVERIFY(!PostsListWidget::isGapItem(&item));

        item.setData(Qt::UserRole, ItemType::gap);
        QVERIFY(!PostsListWidget::isPostItem(&item));
        QVERIFY(PostsListWidget::isGapItem(&item));
    }

    void gapCanBeGeometryOnly()
    {
        QListWidgetItem gap;
        gap.setData(Qt::UserRole, ItemType::gap);
        gap.setData(ItemRole::gapFirstIndex, 80);
        gap.setData(ItemRole::gapCount, 637);
        gap.setFlags(Qt::NoItemFlags);
        gap.setSizeHint(QSize(0, 64000));

        QVERIFY(PostsListWidget::isGapItem(&gap));
        QCOMPARE(gap.text(), QString());
        QVERIFY(gap.icon().isNull());
        QCOMPARE(gap.background().style(), Qt::NoBrush);
        QCOMPARE(gap.flags(), Qt::NoItemFlags);
        QCOMPARE(gap.sizeHint().height(), 64000);
    }
};

QTEST_APPLESS_MAIN(PostsListItemTest)

#include "PostsListItemTest.moc"
