#pragma once

#include <algorithm>

#include <QVector>

namespace Mattermost {

/**
 * Coalesces logical demand around one in-flight exact boundary request.
 *
 * The gate deliberately does not queue transport work. Requests that overlap or
 * directly extend the expected result can attach as waiters. Once the physical
 * request completes, callers release all waiters and let LongListWidget decide
 * which still-missing ranges are still relevant to the current viewport. A
 * distant/random request therefore never waits behind a stale scrolling chain.
 */
class PostSourceRequestGate
{
public:
    struct Range {
        int first = -1;
        int last = -1;
    };

    bool isActive() const { return expected.isValid(); }

    bool canAttach(int first, int last) const
    {
        if (!isActive() || last < first) {
            return false;
        }
        return first <= expected.last + 1 && last >= expected.first - 1;
    }

    void begin(int expectedFirst,
               int expectedLast,
               int requestFirst,
               int requestLast)
    {
        expected.first = std::min(expectedFirst, expectedLast);
        expected.last = std::max(expectedFirst, expectedLast);
        waiters.clear();
        attach(requestFirst, requestLast);
    }

    void attach(int first, int last)
    {
        if (last >= first) {
            waiters.push_back(Range { first, last });
        }
    }

    const Range& expectedRange() const { return expected; }

    QVector<Range> finish()
    {
        QVector<Range> result = std::move(waiters);
        waiters.clear();
        expected = InternalRange {};
        return result;
    }

private:
    struct InternalRange {
        int first = -1;
        int last = -1;

        bool isValid() const { return first >= 0 && last >= first; }
    };

    InternalRange expected;
    QVector<Range> waiters;
};

} // namespace Mattermost
