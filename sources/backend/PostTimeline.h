#pragma once

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
    int logicalIndexForScrollPosition(int value, int minimum, int maximum) const;

    // Return a bounded logical window centred on targetIndex whenever possible.
    LogicalWindow centeredWindow(int targetIndex, int count) const;

    // Fit a window into one existing sparse gap nearest to targetIndex without
    // displacing already materialized posts. Useful for approximate timestamp
    // seeks where the server cannot provide an authoritative logical offset.
    LogicalWindow gapWindowNear(int targetIndex, int count, int minimumIndex = 0) const;

    bool rangeLoaded(int firstIndex, int count) const;

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
    // rows become ordinary sparse gaps again. The returned logical indices let
    // page-based controllers invalidate any page bookkeeping that referred to
    // the evicted materialization.
    QVector<int> pruneLoadedToNearest(int centerIndex, int maxLoadedPosts);

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
    QHash<QString, int> measuredHeights;
};

} // namespace Mattermost
