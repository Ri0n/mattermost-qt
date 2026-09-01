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
        tracker.setMembership(QStringLiteral("channel"), 1000, 5, 5, true, false, false);
        tracker.synchronizeChannel(QStringLiteral("channel"), 2000, 7, 7, true);

        QVERIFY(tracker.isTracked(QStringLiteral("channel")));
        QVERIFY(tracker.isUnread(QStringLiteral("channel")));
        QCOMPARE(tracker.activityTime(QStringLiteral("channel")), uint64_t(2000));
        QCOMPARE(tracker.lastViewedTime(QStringLiteral("channel")), uint64_t(1000));
    }

    void newerActivityTimestampAloneDoesNotCreateUnreadState()
    {
        ChannelActivityTracker tracker;
        tracker.setMembership(QStringLiteral("channel"), 1000, 5, 5, true, false, false);
        tracker.synchronizeChannel(QStringLiteral("channel"), 2000, 5, 5, true);

        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));
        QCOMPARE(tracker.activityTime(QStringLiteral("channel")), uint64_t(2000));
    }

    void recentUsesLastViewedRatherThanLatestTraffic()
    {
        ChannelActivityTracker tracker;
        tracker.setMembership(QStringLiteral("channel"), 1000, 5, 5, true, false, false);
        tracker.recordPost(QStringLiteral("channel"), 3000, false, false, false);

        QCOMPARE(tracker.lastViewedTime(QStringLiteral("channel")), uint64_t(1000));
        QCOMPARE(tracker.activityTime(QStringLiteral("channel")), uint64_t(3000));
    }

    void membershipResponsePreservesRuntimeActivity()
    {
        ChannelActivityTracker tracker;
        tracker.recordPost(QStringLiteral("channel"), 3000, false, false, true);

        // Simulate a membership request that started before the websocket post
        // and therefore returns older read/mention state.
        tracker.setMembership(QStringLiteral("channel"), 1000, 5, 5, true, false, false);
        tracker.synchronizeChannel(QStringLiteral("channel"), 2000, 5, 5, true);

        QVERIFY(tracker.isUnread(QStringLiteral("channel")));
        QCOMPARE(tracker.activityTime(QStringLiteral("channel")), uint64_t(3000));
    }

    void fallsBackToFullCountsWhenRootCounterIsMissingOnChannel()
    {
        ChannelActivityTracker tracker;
        tracker.setMembership(QStringLiteral("channel"), 2000, 10, 5, true, false, false);

        // Some Mattermost versions expose msg_count_root in ChannelMember but
        // not total_msg_count_root in Channel. Comparing 5 against the ordinary
        // total would be a false unread; the compatible full pair is 10/10.
        tracker.synchronizeChannel(QStringLiteral("channel"), 1500, 10, 10, false);

        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));
    }

    void usesRootCountsOnlyWhenBothSidesProvideThem()
    {
        ChannelActivityTracker tracker;
        tracker.setMembership(QStringLiteral("channel"), 1000, 10, 5, true, false, false);

        // Two thread replies advanced the full count, but no new root post.
        tracker.synchronizeChannel(QStringLiteral("channel"), 2000, 12, 5, true);
        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));

        tracker.synchronizeChannel(QStringLiteral("channel"), 2100, 13, 6, true);
        QVERIFY(tracker.isUnread(QStringLiteral("channel")));
    }

    void viewingChannelClearsUnreadAndMakesItRecent()
    {
        ChannelActivityTracker tracker;
        tracker.setMembership(QStringLiteral("channel"), 1000, 5, 5, true, false, false);
        tracker.synchronizeChannel(QStringLiteral("channel"), 2000, 7, 7, true);
        tracker.recordViewed(QStringLiteral("channel"), 3000, 7, 7, true);

        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));
        QCOMPARE(tracker.lastViewedTime(QStringLiteral("channel")), uint64_t(3000));
    }

    void mutedChannelOnlyRequiresAttentionForMention()
    {
        ChannelActivityTracker tracker;
        tracker.setMembership(QStringLiteral("channel"), 1000, 5, 5, true, false, true);
        tracker.synchronizeChannel(QStringLiteral("channel"), 2000, 7, 7, true);

        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));

        tracker.setMentioned(QStringLiteral("channel"), true);
        QVERIFY(tracker.isUnread(QStringLiteral("channel")));

        tracker.setMentioned(QStringLiteral("channel"), false);
        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));

        tracker.setMuted(QStringLiteral("channel"), false);
        QVERIFY(tracker.isUnread(QStringLiteral("channel")));
    }

    void threadReplyNeedsMentionButStillUpdatesActivity()
    {
        ChannelActivityTracker tracker;
        tracker.setMembership(QStringLiteral("channel"), 1000, 5, 5, true, false, false);
        tracker.synchronizeChannel(QStringLiteral("channel"), 1000, 5, 5, true);

        tracker.recordPost(QStringLiteral("channel"), 2000, false, true, false);
        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));
        QCOMPARE(tracker.activityTime(QStringLiteral("channel")), uint64_t(2000));
        QCOMPARE(tracker.lastViewedTime(QStringLiteral("channel")), uint64_t(1000));

        tracker.recordPost(QStringLiteral("channel"), 2100, false, true, true);
        QVERIFY(tracker.isUnread(QStringLiteral("channel")));
    }

    void ownPostDoesNotCreateUnreadState()
    {
        ChannelActivityTracker tracker;
        tracker.setMembership(QStringLiteral("channel"), 1000, 5, 5, true, false, false);
        tracker.synchronizeChannel(QStringLiteral("channel"), 1000, 5, 5, true);

        tracker.recordPost(QStringLiteral("channel"), 2500, true, false, false);

        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));
        QCOMPARE(tracker.activityTime(QStringLiteral("channel")), uint64_t(2500));
        QCOMPARE(tracker.lastViewedTime(QStringLiteral("channel")), uint64_t(1000));
    }
};

QTEST_MAIN(ChannelActivityTrackerTest)

#include "ChannelActivityTrackerTest.moc"
