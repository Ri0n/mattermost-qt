#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <QHash>
#include <QMap>
#include <QStringList>
#include <QVector>

namespace Mattermost {

class PostTimeline
{
public:
    enum SpanKind {
        GapSpan,
        LoadedSpan,
    };

    struct Span {
        SpanKind kind = GapSpan;
        int firstIndex = 0;
        int count = 0;
        QStringList postIds;
        qint64 estimatedHeight = 0;
    };

    struct PixelLocation {
        int logicalIndex = -1;
        QString postId;
        int offsetWithinRow = 0;
        bool loaded = false;

        bool isValid() const { return logicalIndex >= 0; }
    };

    struct LogicalWindow {
        int firstIndex = -1;
        int count = 0;
        int targetIndex = -1;

        bool isValid() const { return firstIndex >= 0 && count > 0; }
        int lastIndex() const { return firstIndex + count - 1; }
    };

    explicit PostTimeline(int initialEstimatedRowHeight = 96);

    void reset(int totalCount = 0);
    void setTotalCount(int totalCount);
    // Channel pages are indexed from the newest edge. When an estimated root
    // count changes, shift existing rows by the delta so the newest edge stays
    // fixed instead of appending/removing capacity on the wrong side.
    void setTotalCountPreservingNewest(int totalCount);
    int totalCount() const { return logicalCount; }
    int loadedCount() const { return loadedByIndex.size(); }

    void placeWindow(int firstIndex, const QStringList& chronologicalPostIds);

    // Map a physical scrollbar position to the logical message index. Random
    // seek intentionally uses normalized thumb position rather than estimated
    // pixel heights: gap-height refinement must not move the logical target
    // while the user is holding the scrollbar thumb.
    int logicalIndexForScrollPosition(int value, int minimum, int maximum) const
    {
        if (logicalCount <= 0) {
            return -1;
        }
        if (logicalCount == 1 || maximum <= minimum) {
            return 0;
        }

        const int clamped = std::max(minimum, std::min(maximum, value));
        const long double fraction = static_cast<long double>(clamped - minimum)
            / static_cast<long double>(maximum - minimum);
        const long double logical = fraction * static_cast<long double>(logicalCount - 1);
        return std::max(0, std::min(logicalCount - 1,
            static_cast<int>(std::llround(logical))));
    }

    // Return a bounded logical window centred on targetIndex whenever possible.
    LogicalWindow centeredWindow(int targetIndex, int count) const
    {
        LogicalWindow result;
        if (logicalCount <= 0 || count <= 0 || targetIndex < 0
            || targetIndex >= logicalCount) {
            return result;
        }

        result.targetIndex = targetIndex;
        result.count = std::min(count, logicalCount);
        const int preferredBefore = (result.count - 1) / 2;
        result.firstIndex = std::max(0, targetIndex - preferredBefore);
        result.firstIndex = std::min(result.firstIndex, logicalCount - result.count);
        return result;
    }

    // Fit a window into one existing sparse gap nearest to targetIndex without
    // displacing already materialized posts. Useful for approximate timestamp
    // seeks where the server cannot provide an authoritative logical offset.
    LogicalWindow gapWindowNear(int targetIndex, int count, int minimumIndex = 0) const
    {
        LogicalWindow result;
        if (logicalCount <= 0 || count <= 0 || targetIndex < 0
            || targetIndex >= logicalCount) {
            return result;
        }

        const int minimum = std::max(0, minimumIndex);
        qint64 bestDistance = -1;
        for (const Span& span : spans()) {
            if (span.kind != GapSpan) {
                continue;
            }
            const int gapFirst = std::max(span.firstIndex, minimum);
            const int gapLastExclusive = span.firstIndex + span.count;
            const int gapCount = gapLastExclusive - gapFirst;
            if (gapCount <= 0) {
                continue;
            }

            const int useCount = std::min(count, gapCount);
            const int maxFirst = gapLastExclusive - useCount;
            const int preferredFirst = targetIndex - (useCount - 1) / 2;
            const int first = std::max(gapFirst, std::min(maxFirst, preferredFirst));
            const int nearest = std::max(first,
                std::min(first + useCount - 1, targetIndex));
            const qint64 distance = std::llabs(static_cast<long long>(nearest)
                                               - static_cast<long long>(targetIndex));
            if (bestDistance < 0 || distance < bestDistance) {
                bestDistance = distance;
                result.firstIndex = first;
                result.count = useCount;
                result.targetIndex = targetIndex;
            }
        }
        return result;
    }

    bool rangeLoaded(int firstIndex, int count) const
    {
        if (count <= 0 || firstIndex < 0 || firstIndex + count > logicalCount) {
            return false;
        }
        for (int index = firstIndex; index < firstIndex + count; ++index) {
            if (postIdAt(index).isEmpty()) {
                return false;
            }
        }
        return true;
    }

    // Return the real contiguous loaded span containing logicalIndex. Seek
    // expansion uses this after every server block so already materialized rows
    // are merged naturally instead of being treated as separate windows.
    LogicalWindow loadedWindowContaining(int logicalIndex) const
    {
        LogicalWindow result;
        if (logicalIndex < 0 || logicalIndex >= logicalCount) {
            return result;
        }
        for (const Span& span : spans()) {
            if (span.kind != LoadedSpan
                || logicalIndex < span.firstIndex
                || logicalIndex >= span.firstIndex + span.count) {
                continue;
            }
            result.firstIndex = span.firstIndex;
            result.count = span.count;
            result.targetIndex = logicalIndex;
            return result;
        }
        return result;
    }

    // A seek should cover the viewport plus roughly one viewport of read-ahead
    // on each side. Keep a useful minimum (the normal 10+10+10 transaction) and
    // cap refinement so a pathological tiny-row layout cannot run away.
    int rowsForViewportCoverage(int viewportHeight,
                                int bufferScreens = 1,
                                int minimumRows = 30,
                                int maximumRows = 90) const
    {
        const int rowHeight = std::max(1, estimatedRowHeight());
        const int screens = 1 + 2 * std::max(0, bufferScreens);
        const qint64 pixels = static_cast<qint64>(std::max(0, viewportHeight)) * screens;
        const int byHeight = static_cast<int>((pixels + rowHeight - 1) / rowHeight);
        return std::max(minimumRows, std::min(maximumRows, byHeight));
    }

    // A cursor response can prove that a loaded span touches the real oldest or
    // newest edge even when that span was originally placed approximately in a
    // sparse gap. Move the whole contiguous span to the authoritative edge and
    // leave the remaining uncertainty on the opposite side.
    bool alignLoadedSpanToBoundary(const QString& postId, bool oldestBoundary);

    // Return the adjacent unloaded logical index once the viewport is within
    // maxLoadedRowsBeforeGap real messages of a gap. This is deliberately row-
    // based rather than pixel/screen based so variable post heights do not defer
    // prefetch until the user has already entered the placeholder.
    int adjacentGapIndex(int loadedIndex,
                         bool olderDirection,
                         int maxLoadedRowsBeforeGap) const;

    // Keep only the maxLoadedPosts logical rows nearest to centerIndex. Removed
    // rows become ordinary sparse gaps again. Measured row heights survive this
    // UI eviction: they are identity metadata used to keep future gap geometry
    // stable and are cleared only by reset().
    QVector<int> pruneLoadedToNearest(int centerIndex, int maxLoadedPosts);

    // Same pruning policy, but with a hard logical protection range. This is the
    // UI contract used by both channel and thread controllers: visible rows plus
    // their safety margin are never evicted, even if the protected set alone
    // temporarily exceeds the nominal materialization budget.
    QVector<int> pruneLoadedToNearest(int centerIndex,
                                      int maxLoadedPosts,
                                      int protectedFirstIndex,
                                      int protectedLastIndex);

    bool contains(const QString& postId) const;
    int indexOf(const QString& postId) const;
    QString postIdAt(int logicalIndex) const;

    QVector<Span> spans() const;

    void recordMeasuredHeight(const QString& postId, int height);
    int estimatedRowHeight() const;
    qint64 estimatedTotalHeight() const;
    qint64 estimatedPixelForIndex(int logicalIndex) const;
    PixelLocation locatePixel(qint64 pixelOffset) const;

private:
    void rebuildMeasuredHeightStats();
    int rowHeight(const QString& postId) const;
    qint64 estimatedGapHeight(int count) const;

    int logicalCount = 0;
    int initialRowHeight = 96;
    qint64 measuredHeightSum = 0;
    int measuredHeightCount = 0;
    QMap<int, QString> loadedByIndex;
    QHash<QString, int> indexByPostId;
    // Small identity-level geometry cache for the lifetime of this timeline.
    // UI pruning must not make all sparse gaps change height by forgetting it.
    QHash<QString, int> measuredHeights;
};

} // namespace Mattermost
