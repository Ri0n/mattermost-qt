#include <QtTest>

#include "backend/ChannelActivityTracker.h"

using namespace Mattermost;

namespace {

void setMembership(ChannelActivityTracker& tracker,
                   uint64_t lastViewedAt = 1000,
                   uint64_t readMessages = 5,
                   uint64_t readRootMessages = 5,
                   bool hasRootMessages = true,
                   uint64_t mentions = 0,
                   uint64_t rootMentions = 0,
                   bool hasRootMentions = true,
                   bool muted = false)
{
    tracker.setMembership(QStringLiteral("channel"), lastViewedAt,
                          readMessages, readRootMessages, hasRootMessages,
                          mentions, rootMentions, hasRootMentions, muted);
}

void synchronize(ChannelActivityTracker& tracker,
                 uint64_t lastPostAt = 2000,
                 uint64_t totalMessages = 5,
                 uint64_t totalRootMessages = 5,
                 bool hasRootMessages = true,
                 bool collapsedThreads = false)
{
    tracker.synchronizeChannel(QStringLiteral("channel"), lastPostAt,
                               totalMessages, totalRootMessages,
                               hasRootMessages, collapsedThreads);
}

} // namespace

class ChannelActivityTrackerTest : public QObject
{
    Q_OBJECT

private slots:
    void ordinaryCountsSeedUnreadWhenCrtIsOff()
    {
        ChannelActivityTracker tracker;
        setMembership(tracker, 1000, 5, 5);
        synchronize(tracker, 2000, 7, 5, true, false);

        QVERIFY(tracker.isTracked(QStringLiteral("channel")));
        QVERIFY(tracker.isUnread(QStringLiteral("channel")));
    }

    void rootCountsSeedUnreadWhenCrtIsOn()
    {
        ChannelActivityTracker tracker;
        setMembership(tracker, 1000, 20, 5);
        synchronize(tracker, 2000, 20, 7, true, true);

        QVERIFY(tracker.isUnread(QStringLiteral("channel")));
    }

    void rootFieldsDoNotChangeUnreadSemanticsWhenCrtIsOff()
    {
        ChannelActivityTracker tracker;
        setMembership(tracker, 1000, 10, 5);

        // The ordinary channel has two unread messages while its root counters
        // happen to be equal. Merely having CRT fields in JSON must not switch
        // the comparison domain when Collapsed Reply Threads is disabled.
        synchronize(tracker, 2000, 12, 5, true, false);
        QVERIFY(tracker.isUnread(QStringLiteral("channel")));

        tracker.recordViewed(QStringLiteral("channel"), 2000, 12, 5, true);
        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));

        // With CRT enabled the same root counters correctly say there is no
        // unread root activity.
        synchronize(tracker, 2000, 12, 5, true, true);
        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));
    }

    void crtUsesRootMentionCount()
    {
        ChannelActivityTracker tracker;
        setMembership(tracker, 1000, 5, 5, true, 3, 0, true, false);

        synchronize(tracker, 1000, 5, 5, true, true);
        QVERIFY(!tracker.hasMention(QStringLiteral("channel")));
        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));

        synchronize(tracker, 1000, 5, 5, true, false);
        QVERIFY(tracker.hasMention(QStringLiteral("channel")));
        QVERIFY(tracker.isUnread(QStringLiteral("channel")));
    }

    void membershipResponsePreservesRuntimeActivity()
    {
        ChannelActivityTracker tracker;
        tracker.recordPost(QStringLiteral("channel"), 3000, false, false, true);

        // Simulate a membership response that was requested before the
        // websocket post and therefore contains older server state.
        setMembership(tracker, 1000, 5, 5, true, 0, 0, true, false);
        synchronize(tracker, 2000, 5, 5, true, false);

        QVERIFY(tracker.isUnread(QStringLiteral("channel")));
        QVERIFY(tracker.hasMention(QStringLiteral("channel")));
        QCOMPARE(tracker.activityTime(QStringLiteral("channel")), uint64_t(3000));
    }

    void viewingChannelClearsServerAndRuntimeUnread()
    {
        ChannelActivityTracker tracker;
        setMembership(tracker, 1000, 5, 5);
        synchronize(tracker, 2000, 7, 7, true, false);
        tracker.recordPost(QStringLiteral("channel"), 2500, false, false, true);

        QVERIFY(tracker.isUnread(QStringLiteral("channel")));
        QVERIFY(tracker.hasMention(QStringLiteral("channel")));

        tracker.recordViewed(QStringLiteral("channel"), 2500, 8, 8, true);

        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));
        QVERIFY(!tracker.hasMention(QStringLiteral("channel")));
    }

    void reopeningReadChannelDoesNotAdvanceRecentTime()
    {
        ChannelActivityTracker tracker;
        setMembership(tracker, 2000, 7, 7);
        synchronize(tracker, 2000, 7, 7, true, false);

        QCOMPARE(tracker.recentTime(QStringLiteral("channel")), uint64_t(2000));

        // Viewing the same already-read content again uses the channel's last
        // post timestamp, not wall-clock time, so recency is stable.
        tracker.recordViewed(QStringLiteral("channel"), 2000, 7, 7, true);
        QCOMPARE(tracker.recentTime(QStringLiteral("channel")), uint64_t(2000));
    }

    void recentTimeIncludesMattermostRecencyPreferences()
    {
        ChannelActivityTracker tracker;
        setMembership(tracker, 1000, 5, 5);
        tracker.setRecencyTimes(QStringLiteral("channel"), 2000, 3000);

        QCOMPARE(tracker.lastViewedTime(QStringLiteral("channel")), uint64_t(1000));
        QCOMPARE(tracker.recentTime(QStringLiteral("channel")), uint64_t(3000));

        tracker.recordViewed(QStringLiteral("channel"), 2500, 5, 5, true);
        QCOMPARE(tracker.recentTime(QStringLiteral("channel")), uint64_t(3000));
    }

    void mutedChannelOnlyRequiresAttentionForMention()
    {
        ChannelActivityTracker tracker;
        setMembership(tracker, 1000, 5, 5, true, 0, 0, true, true);
        synchronize(tracker, 2000, 7, 7, true, false);

        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));

        tracker.setMentioned(QStringLiteral("channel"), true);
        QVERIFY(tracker.isUnread(QStringLiteral("channel")));

        tracker.setMentioned(QStringLiteral("channel"), false);
        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));

        tracker.setMuted(QStringLiteral("channel"), false);
        QVERIFY(tracker.isUnread(QStringLiteral("channel")));
    }

    void threadReplyBelongsToChannelWhenCrtIsOff()
    {
        ChannelActivityTracker tracker;
        setMembership(tracker, 1000, 5, 5);
        synchronize(tracker, 1000, 5, 5, true, false);

        tracker.recordPost(QStringLiteral("channel"), 2000, false, true, false);
        QVERIFY(tracker.isUnread(QStringLiteral("channel")));
        QCOMPARE(tracker.activityTime(QStringLiteral("channel")), uint64_t(2000));
    }

    void threadReplyStaysOutOfParentChannelWhenCrtIsOn()
    {
        ChannelActivityTracker tracker;
        setMembership(tracker, 1000, 5, 5);
        synchronize(tracker, 1000, 5, 5, true, true);

        tracker.recordPost(QStringLiteral("channel"), 2000, false, true, false);
        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));
        QCOMPARE(tracker.activityTime(QStringLiteral("channel")), uint64_t(2000));

        // A mention in a CRT reply belongs to the followed thread's unread
        // state, not to the parent channel's mention_count_root.
        tracker.recordPost(QStringLiteral("channel"), 2100, false, true, true);
        QVERIFY(!tracker.hasMention(QStringLiteral("channel")));
        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));
    }

    void ownPostDoesNotCreateUnreadState()
    {
        ChannelActivityTracker tracker;
        setMembership(tracker, 1000, 5, 5);
        synchronize(tracker, 1000, 5, 5, true, false);

        tracker.recordPost(QStringLiteral("channel"), 2500, true, false, false);

        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));
        QCOMPARE(tracker.activityTime(QStringLiteral("channel")), uint64_t(2500));
    }
};

QTEST_MAIN(ChannelActivityTrackerTest)

#include "ChannelActivityTrackerTest.moc"
