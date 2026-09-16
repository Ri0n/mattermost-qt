#include <QtTest>

#include "backend/PublicChannelPaging.h"

using namespace Mattermost;

class PublicChannelPagingTest : public QObject
{
    Q_OBJECT
private slots:
    void requestUsesExplicitPageSize()
    {
        QCOMPARE(publicChannelsPagePath(QStringLiteral("team-id"), 3),
                 QStringLiteral("teams/team-id/channels?page=3&per_page=100"));
    }

    void fullPageExposesOneProvisionalPageOnly()
    {
        QCOMPARE(publicChannelLogicalCountAfterPage(0, PublicChannelsPerPage), 200);
        QCOMPARE(publicChannelLogicalCountAfterPage(4, PublicChannelsPerPage), 600);
        QVERIFY(!publicChannelPageProvesEnd(PublicChannelsPerPage));
    }

    void shortPageProvesExactEnd()
    {
        QCOMPARE(publicChannelLogicalCountAfterPage(4, 17), 417);
        QVERIFY(publicChannelPageProvesEnd(17));
        QVERIFY(publicChannelPageProvesEnd(0));
    }
};

QTEST_APPLESS_MAIN(PublicChannelPagingTest)
#include "PublicChannelPagingTest.moc"
