#include "PostTimeline.h"

#include <algorithm>
#include <utility>

#include <QSet>

namespace Mattermost {

PostTimeline::PostTimeline(int initialEstimatedRowHeight)
    : initialRowHeight(std::max(1, initialEstimatedRowHeight))
{
}

void PostTimeline::reset(int totalCount)
{
    logicalCount = std::max(0, totalCount);
    loadedByIndex.clear();
    indexByPostId.clear();
    measuredHeights.clear();
    measuredHeightSum = 0;
    measuredHeightCount = 0;
}

void PostTimeline::rebuildMeasuredHeightStats()
{
    measuredHeightSum = 0;
    measuredHeightCount = 0;
    for (auto it = measuredHeights.cbegin(); it != measuredHeights.cend(); ++it) {
        measuredHeightSum += it.value();
        ++measuredHeightCount;
    }
}

void PostTimeline::setTotalCount(int totalCount)
{
    const int newCount = std::max(0, totalCount);
    if (newCount == logicalCount) {
        return;
    }

    logicalCount = newCount;
    for (auto it = loadedByIndex.lowerBound(logicalCount); it != loadedByIndex.end();) {
        indexByPostId.remove(it.value());
        measuredHeights.remove(it.value());
        it = loadedByIndex.erase(it);
    }
    rebuildMeasuredHeightStats();
}

void PostTimeline::setTotalCountPreservingNewest(int totalCount)
{
    const int newCount = std::max(0, totalCount);
    if (newCount == logicalCount) {
        return;
    }

    const int delta = newCount - logicalCount;
    QMap<int, QString> shifted;
    QHash<QString, int> shiftedIndexByPostId;
    QHash<QString, int> keptHeights;

    for (auto it = loadedByIndex.cbegin(); it != loadedByIndex.cend(); ++it) {
        const int newIndex = it.key() + delta;
        if (newIndex < 0 || newIndex >= newCount) {
            continue;
        }
        shifted.insert(newIndex, it.value());
        shiftedIndexByPostId.insert(it.value(), newIndex);
        const auto heightIt = measuredHeights.constFind(it.value());
        if (heightIt != measuredHeights.cend()) {
            keptHeights.insert(it.value(), heightIt.value());
        }
    }

    logicalCount = newCount;
    loadedByIndex = std::move(shifted);
    indexByPostId = std::move(shiftedIndexByPostId);
    measuredHeights = std::move(keptHeights);
    rebuildMeasuredHeightStats();
}

void PostTimeline::placeWindow(int firstIndex, const QStringList& chronologicalPostIds)
{
    if (chronologicalPostIds.isEmpty() || logicalCount <= 0) {
        return;
    }

    const int sourceCount = static_cast<int>(chronologicalPostIds.size());
    int sourceOffset = 0;
    int target = firstIndex;
    if (target < 0) {
        sourceOffset = -target;
        target = 0;
    }
    if (sourceOffset >= sourceCount || target >= logicalCount) {
        return;
    }

    const int available = std::min(sourceCount - sourceOffset,
                                   logicalCount - target);

    auto forgetPost = [this](const QString& postId) {
        indexByPostId.remove(postId);
        const auto measured = measuredHeights.find(postId);
        if (measured != measuredHeights.end()) {
            measuredHeightSum -= measured.value();
            --measuredHeightCount;
            measuredHeights.erase(measured);
        }
    };

    for (int i = 0; i < available; ++i) {
        const QString& postId = chronologicalPostIds.at(sourceOffset + i);
        if (postId.isEmpty()) {
            continue;
        }
        const auto oldIndexIt = indexByPostId.constFind(postId);
        if (oldIndexIt != indexByPostId.cend()) {
            loadedByIndex.remove(oldIndexIt.value());
            indexByPostId.remove(postId);
        }
    }

    for (int i = 0; i < available; ++i) {
        const QString& postId = chronologicalPostIds.at(sourceOffset + i);
        if (postId.isEmpty()) {
            continue;
        }

        const int logicalIndex = target + i;
        const auto collision = loadedByIndex.find(logicalIndex);
        if (collision != loadedByIndex.end() && collision.value() != postId) {
            const QString displacedId = collision.value();
            loadedByIndex.erase(collision);
            forgetPost(displacedId);
        }

        loadedByIndex.insert(logicalIndex, postId);
        indexByPostId.insert(postId, logicalIndex);
    }
}

bool PostTimeline::alignLoadedSpanToBoundary(const QString& postId, bool oldestBoundary)
{
    const int logicalIndex = indexOf(postId);
    if (logicalIndex < 0 || logicalCount <= 0) {
        return false;
    }

    for (const Span& span : spans()) {
        if (span.kind != LoadedSpan
            || logicalIndex < span.firstIndex
            || logicalIndex >= span.firstIndex + span.count
            || span.postIds.isEmpty()) {
            continue;
        }

        const int firstIndex = oldestBoundary
            ? 0
            : std::max(0, logicalCount - span.count);
        if (span.firstIndex != firstIndex) {
            placeWindow(firstIndex, span.postIds);
        }
        return true;
    }
    return false;
}

int PostTimeline::adjacentGapIndex(int loadedIndex,
                                   bool olderDirection,
                                   int maxLoadedRowsBeforeGap) const
{
    if (loadedIndex < 0 || loadedIndex >= logicalCount
        || postIdAt(loadedIndex).isEmpty()) {
        return -1;
    }

    const int threshold = std::max(0, maxLoadedRowsBeforeGap);
    for (const Span& span : spans()) {
        if (span.kind != LoadedSpan
            || loadedIndex < span.firstIndex
            || loadedIndex >= span.firstIndex + span.count) {
            continue;
        }

        if (olderDirection) {
            if (span.firstIndex == 0 || loadedIndex - span.firstIndex > threshold) {
                return -1;
            }
            const int gapIndex = span.firstIndex - 1;
            return postIdAt(gapIndex).isEmpty() ? gapIndex : -1;
        }

        const int spanLastIndex = span.firstIndex + span.count - 1;
        if (spanLastIndex >= logicalCount - 1
            || spanLastIndex - loadedIndex > threshold) {
            return -1;
        }
        const int gapIndex = spanLastIndex + 1;
        return postIdAt(gapIndex).isEmpty() ? gapIndex : -1;
    }
    return -1;
}

QVector<int> PostTimeline::pruneLoadedToNearest(int centerIndex, int maxLoadedPosts)
{
    QVector<int> removed;
    const int limit = std::max(0, maxLoadedPosts);
    if (loadedByIndex.size() <= limit) {
        return removed;
    }

    const int center = logicalCount > 0
        ? std::max(0, std::min(logicalCount - 1, centerIndex))
        : 0;

    struct Candidate {
        int index = 0;
        qint64 distance = 0;
    };

    QVector<Candidate> candidates;
    candidates.reserve(loadedByIndex.size());
    for (auto it = loadedByIndex.cbegin(); it != loadedByIndex.cend(); ++it) {
        Candidate candidate;
        candidate.index = it.key();
        candidate.distance = std::abs(static_cast<qint64>(it.key()) - center);
        candidates.push_back(candidate);
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& lhs,
                                                       const Candidate& rhs) {
        if (lhs.distance != rhs.distance) {
            return lhs.distance < rhs.distance;
        }
        return lhs.index < rhs.index;
    });

    QSet<int> keep;
    keep.reserve(limit);
    const int candidateCount = static_cast<int>(candidates.size());
    for (int i = 0; i < limit && i < candidateCount; ++i) {
        keep.insert(candidates.at(i).index);
    }

    for (auto it = loadedByIndex.begin(); it != loadedByIndex.end();) {
        if (keep.contains(it.key())) {
            ++it;
            continue;
        }

        const int logicalIndex = it.key();
        const QString postId = it.value();
        removed.push_back(logicalIndex);
        indexByPostId.remove(postId);

        const auto measured = measuredHeights.find(postId);
        if (measured != measuredHeights.end()) {
            measuredHeightSum -= measured.value();
            --measuredHeightCount;
            measuredHeights.erase(measured);
        }

        it = loadedByIndex.erase(it);
    }

    std::sort(removed.begin(), removed.end());
    return removed;
}

bool PostTimeline::contains(const QString& postId) const
{
    return indexByPostId.contains(postId);
}

int PostTimeline::indexOf(const QString& postId) const
{
    const auto it = indexByPostId.constFind(postId);
    return it == indexByPostId.cend() ? -1 : it.value();
}

QString PostTimeline::postIdAt(int logicalIndex) const
{
    return loadedByIndex.value(logicalIndex);
}

QVector<PostTimeline::Span> PostTimeline::spans() const
{
    QVector<Span> result;
    if (logicalCount <= 0) {
        return result;
    }

    int cursor = 0;
    auto it = loadedByIndex.cbegin();
    while (it != loadedByIndex.cend()) {
        if (it.key() > cursor) {
            Span gap;
            gap.kind = GapSpan;
            gap.firstIndex = cursor;
            gap.count = it.key() - cursor;
            gap.estimatedHeight = estimatedGapHeight(gap.count);
            result.push_back(std::move(gap));
        }

        Span loaded;
        loaded.kind = LoadedSpan;
        loaded.firstIndex = it.key();
        int expectedIndex = it.key();
        while (it != loadedByIndex.cend() && it.key() == expectedIndex) {
            loaded.postIds.push_back(it.value());
            loaded.estimatedHeight += rowHeight(it.value());
            ++expectedIndex;
            ++it;
        }
        loaded.count = static_cast<int>(loaded.postIds.size());
        result.push_back(std::move(loaded));
        cursor = expectedIndex;
    }

    if (cursor < logicalCount) {
        Span gap;
        gap.kind = GapSpan;
        gap.firstIndex = cursor;
        gap.count = logicalCount - cursor;
        gap.estimatedHeight = estimatedGapHeight(gap.count);
        result.push_back(std::move(gap));
    }

    return result;
}

void PostTimeline::recordMeasuredHeight(const QString& postId, int height)
{
    if (postId.isEmpty() || height <= 0 || !indexByPostId.contains(postId)) {
        return;
    }

    const auto existing = measuredHeights.find(postId);
    if (existing != measuredHeights.end()) {
        measuredHeightSum -= existing.value();
        existing.value() = height;
        measuredHeightSum += height;
        return;
    }

    measuredHeights.insert(postId, height);
    measuredHeightSum += height;
    ++measuredHeightCount;
}

int PostTimeline::estimatedRowHeight() const
{
    if (measuredHeightCount <= 0) {
        return initialRowHeight;
    }
    return std::max(1, static_cast<int>(measuredHeightSum / measuredHeightCount));
}

qint64 PostTimeline::estimatedTotalHeight() const
{
    qint64 height = 0;
    int loadedCountSeen = 0;
    for (auto it = loadedByIndex.cbegin(); it != loadedByIndex.cend(); ++it) {
        height += rowHeight(it.value());
        ++loadedCountSeen;
    }
    height += estimatedGapHeight(logicalCount - loadedCountSeen);
    return height;
}

qint64 PostTimeline::estimatedPixelForIndex(int logicalIndex) const
{
    if (logicalCount <= 0 || logicalIndex <= 0) {
        return 0;
    }
    const int clampedIndex = std::min(logicalIndex, logicalCount);

    qint64 y = 0;
    int cursor = 0;
    for (auto it = loadedByIndex.cbegin(); it != loadedByIndex.cend() && cursor < clampedIndex; ++it) {
        if (it.key() > cursor) {
            const int gapEnd = std::min(it.key(), clampedIndex);
            y += estimatedGapHeight(gapEnd - cursor);
            cursor = gapEnd;
            if (cursor >= clampedIndex) {
                break;
            }
        }

        if (it.key() == cursor) {
            y += rowHeight(it.value());
            ++cursor;
        }
    }

    if (cursor < clampedIndex) {
        y += estimatedGapHeight(clampedIndex - cursor);
    }
    return y;
}

PostTimeline::PixelLocation PostTimeline::locatePixel(qint64 pixelOffset) const
{
    PixelLocation result;
    if (logicalCount <= 0) {
        return result;
    }

    const qint64 totalHeight = estimatedTotalHeight();
    qint64 remaining = std::max<qint64>(0, pixelOffset);
    if (totalHeight > 0) {
        remaining = std::min(remaining, totalHeight - 1);
    }

    int cursor = 0;
    for (auto it = loadedByIndex.cbegin(); it != loadedByIndex.cend(); ++it) {
        if (it.key() > cursor) {
            const int gapCount = it.key() - cursor;
            const int average = estimatedRowHeight();
            const qint64 gapHeight = static_cast<qint64>(gapCount) * average;
            if (remaining < gapHeight) {
                const int offsetRows = static_cast<int>(remaining / average);
                result.logicalIndex = cursor + std::min(offsetRows, gapCount - 1);
                result.offsetWithinRow = static_cast<int>(remaining % average);
                return result;
            }
            remaining -= gapHeight;
            cursor = it.key();
        }

        const int height = rowHeight(it.value());
        if (remaining < height) {
            result.logicalIndex = it.key();
            result.postId = it.value();
            result.offsetWithinRow = static_cast<int>(remaining);
            result.loaded = true;
            return result;
        }
        remaining -= height;
        cursor = it.key() + 1;
    }

    const int average = estimatedRowHeight();
    const int gapCount = logicalCount - cursor;
    if (gapCount > 0) {
        const int offsetRows = static_cast<int>(remaining / average);
        result.logicalIndex = cursor + std::min(offsetRows, gapCount - 1);
        result.offsetWithinRow = static_cast<int>(remaining % average);
        return result;
    }

    result.logicalIndex = logicalCount - 1;
    result.postId = loadedByIndex.value(result.logicalIndex);
    result.loaded = !result.postId.isEmpty();
    return result;
}

int PostTimeline::rowHeight(const QString& postId) const
{
    return measuredHeights.value(postId, estimatedRowHeight());
}

qint64 PostTimeline::estimatedGapHeight(int count) const
{
    return static_cast<qint64>(std::max(0, count)) * estimatedRowHeight();
}

} // namespace Mattermost
