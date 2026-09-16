#include <QtTest>

#include "channel-tree/SidebarChannelMovePolicy.h"

using namespace Mattermost;

class SidebarChannelMovePolicyTest : public QObject
{
    Q_OBJECT
private slots:
    void reordersBySemanticChannelIdentity()
    {
        QStringList ids {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")};
        QVERIFY(reorderSidebarChannel(ids, QStringLiteral("c"), QStringLiteral("a"), false));
        QCOMPARE(ids, QStringList({QStringLiteral("c"), QStringLiteral("a"), QStringLiteral("b")}));
    }

    void reordersAfterTarget()
    {
        QStringList ids {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")};
        QVERIFY(reorderSidebarChannel(ids, QStringLiteral("a"), QStringLiteral("b"), true));
        QCOMPARE(ids, QStringList({QStringLiteral("b"), QStringLiteral("a"), QStringLiteral("c")}));
    }

    void movesBetweenCategoriesWithoutDuplicates()
    {
        QStringList source {QStringLiteral("a"), QStringLiteral("b")};
        QStringList target {QStringLiteral("c"), QStringLiteral("a"), QStringLiteral("d")};
        QVERIFY(moveSidebarChannel(source, target, QStringLiteral("a"), QStringLiteral("d"), false));
        QCOMPARE(source, QStringList({QStringLiteral("b")}));
        QCOMPARE(target, QStringList({QStringLiteral("c"), QStringLiteral("a"), QStringLiteral("d")}));
    }

    void categoryHeaderDropAppends()
    {
        QStringList source {QStringLiteral("a"), QStringLiteral("b")};
        QStringList target {QStringLiteral("c"), QStringLiteral("d")};
        QVERIFY(moveSidebarChannel(source, target, QStringLiteral("a")));
        QCOMPARE(source, QStringList({QStringLiteral("b")}));
        QCOMPARE(target, QStringList({QStringLiteral("c"), QStringLiteral("d"), QStringLiteral("a")}));
    }
};

QTEST_APPLESS_MAIN(SidebarChannelMovePolicyTest)
#include "SidebarChannelMovePolicyTest.moc"
