#include <QtTest>

#include "backend/ChannelActivityTracker.h"

using namespace Mattermost;

class ChannelActivityTrackerTest : public QObject
{
    Q_OBJECT

private slots:
    void serverMembershipSeedsUnreadState()
    {
        ChannelActivityTracker tracker;
        tracker.setMembership(QStringLiteral("channel"), 1000, 5, false, false);
        tracker.synchronizeChannel(QStringLiteral("channel"), 2000, 7);

        QVERIFY(tracker.isTracked(QStringLiteral("channel")));
        QVERIFY(tracker.isUnread(QStringLiteral("channel")));
        QCOMPARE(tracker.activityTime(QStringLiteral("channel")), quint64(2000));
    }

    void viewingChannelClearsUnreadAndMakesItRecent()
    {
        ChannelActivityTracker tracker;
        tracker.setMembership(QStringLiteral("channel"), 1000, 5, false, false);
        tracker.synchronizeChannel(QStringLiteral("channel"), 2000, 7);
        tracker.recordViewed(QStringLiteral("channel"), 3000, 7);

        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));
        QCOMPARE(tracker.activityTime(QStringLiteral("channel")), quint64(3000));
    }

    void mutedChannelOnlyRequiresAttentionForMention()
    {
        ChannelActivityTracker tracker;
        tracker.setMembership(QStringLiteral("channel"), 1000, 5, false, true);
        tracker.synchronizeChannel(QStringLiteral("channel"), 2000, 7);

        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));

        tracker.setMentioned(QStringLiteral("channel"), true);
        QVERIFY(tracker.isUnread(QStringLiteral("channel")));

        tracker.setMentioned(QStringLiteral("channel"), false);
        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));

        tracker.setMuted(QStringLiteral("channel"), false);
        QVERIFY(tracker.isUnread(QStringLiteral("channel")));
    }

    void threadReplyNeedsMentionButStillUpdatesRecency()
    {
        ChannelActivityTracker tracker;
        tracker.setMembership(QStringLiteral("channel"), 1000, 5, false, false);
        tracker.synchronizeChannel(QStringLiteral("channel"), 1000, 5);

        tracker.recordPost(QStringLiteral("channel"), 2000, false, true, false);
        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));
        QCOMPARE(tracker.activityTime(QStringLiteral("channel")), quint64(2000));

        tracker.recordPost(QStringLiteral("channel"), 2100, false, true, true);
        QVERIFY(tracker.isUnread(QStringLiteral("channel")));
    }

    void ownPostDoesNotCreateUnreadState()
    {
        ChannelActivityTracker tracker;
        tracker.setMembership(QStringLiteral("channel"), 1000, 5, false, false);
        tracker.synchronizeChannel(QStringLiteral("channel"), 1000, 5);

        tracker.recordPost(QStringLiteral("channel"), 2500, true, false, false);

        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));
        QCOMPARE(tracker.activityTime(QStringLiteral("channel")), quint64(2500));
    }
};

QTEST_MAIN(ChannelActivityTrackerTest)

#include "ChannelActivityTrackerTest.moc"
