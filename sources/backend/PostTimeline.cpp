#include "PostTimeline.h"

#include <algorithm>
#include <utility>

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

    measuredHeightSum = 0;
    measuredHeightCount = 0;
    for (auto it = measuredHeights.cbegin(); it != measuredHeights.cend(); ++it) {
        measuredHeightSum += it.value();
        ++measuredHeightCount;
    }
}

void PostTimeline::placeWindow(int firstIndex, const QStringList& chronologicalPostIds)
{
    if (chronologicalPostIds.isEmpty() || logicalCount <= 0) {
        return;
    }

    int sourceOffset = 0;
    int target = firstIndex;
    if (target < 0) {
        sourceOffset = -target;
        target = 0;
    }
    if (sourceOffset >= chronologicalPostIds.size() || target >= logicalCount) {
        return;
    }

    const int available = std::min(chronologicalPostIds.size() - sourceOffset,
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

    // First remove previous occurrences of every incoming ID. Keep its measured
    // height: relocating an already-rendered post does not invalidate geometry
    // learned for the post itself.
    QHash<QString, int> incomingMeasuredHeights;
    for (int i = 0; i < available; ++i) {
        const QString& postId = chronologicalPostIds.at(sourceOffset + i);
        if (postId.isEmpty()) {
            continue;
        }
        const auto oldIndexIt = indexByPostId.constFind(postId);
        if (oldIndexIt == indexByPostId.cend()) {
            continue;
        }
        loadedByIndex.remove(oldIndexIt.value());
        indexByPostId.remove(postId);
        const auto measured = measuredHeights.constFind(postId);
        if (measured != measuredHeights.cend()) {
            incomingMeasuredHeights.insert(postId, measured.value());
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

    // Relocation preserved existing entries in measuredHeights and therefore
    // needs no sum adjustment. The hash only documents that intent and avoids a
    // future implementation accidentally treating relocation as replacement.
    Q_UNUSED(incomingMeasuredHeights);
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
        loaded.count = loaded.postIds.size();
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
