#include <QtTest>

#include "backend/PostWindowSelection.h"

using namespace Mattermost;

namespace {

QStringList postIds(int count)
{
    QStringList ids;
    ids.reserve(count);
    for (int i = 0; i < count; ++i) {
        ids.push_back(QStringLiteral("p%1").arg(i));
    }
    return ids;
}

} // namespace

class PostWindowSelectionTest : public QObject
{
    Q_OBJECT

private slots:
    void centersWhenBothSidesAreAvailable()
    {
        const QStringList ids = postIds(100);
        const QStringList window = selectPostWindow(ids, QStringLiteral("p50"), 31);

        QCOMPARE(window.size(), 31);
        QCOMPARE(window.first(), QStringLiteral("p35"));
        QCOMPARE(window.at(15), QStringLiteral("p50"));
        QCOMPARE(window.last(), QStringLiteral("p65"));
    }

    void spillsUnusedNewestQuotaIntoOlderSide()
    {
        const QStringList ids = postIds(100);
        const QStringList window = selectPostWindow(ids, QStringLiteral("p97"), 31);

        QCOMPARE(window.size(), 31);
        QCOMPARE(window.first(), QStringLiteral("p69"));
        QCOMPARE(window.at(28), QStringLiteral("p97"));
        QCOMPARE(window.last(), QStringLiteral("p99"));
    }

    void spillsUnusedOldestQuotaIntoNewerSide()
    {
        const QStringList ids = postIds(100);
        const QStringList window = selectPostWindow(ids, QStringLiteral("p2"), 31);

        QCOMPARE(window.size(), 31);
        QCOMPARE(window.first(), QStringLiteral("p0"));
        QCOMPARE(window.at(2), QStringLiteral("p2"));
        QCOMPARE(window.last(), QStringLiteral("p30"));
    }

    void usesAllAvailablePostsWhenTimelineIsShort()
    {
        const QStringList ids = postIds(9);
        const QStringList window = selectPostWindow(ids, QStringLiteral("p7"), 31);

        QCOMPARE(window, ids);
    }

    void rejectsMissingTarget()
    {
        QVERIFY(selectPostWindow(postIds(10), QStringLiteral("missing"), 31).isEmpty());
    }

    void neverExceedsRequestedCount()
    {
        const QStringList ids = postIds(100);
        const QStringList window = selectPostWindow(ids, QStringLiteral("p50"), 7);

        QCOMPARE(window.size(), 7);
        QCOMPARE(window.first(), QStringLiteral("p47"));
        QCOMPARE(window.last(), QStringLiteral("p53"));
    }
};

QTEST_APPLESS_MAIN(PostWindowSelectionTest)

#include "PostWindowSelectionTest.moc"
