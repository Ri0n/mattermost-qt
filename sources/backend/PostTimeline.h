/**
 * @file PostTimeline.h
 * @brief Sparse logical post timeline used by virtualized channel/thread views.
 *
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <QHash>
#include <QMap>
#include <QStringList>
#include <QVector>

namespace Mattermost {

/**
 * Logical ordering of a potentially very large post stream.
 *
 * Only loaded posts occupy memory. Missing ranges are represented implicitly
 * as gaps whose pixel size is estimated from measured post heights. This keeps
 * scrollbar geometry useful before every post is downloaded and provides the
 * mapping needed to seek into an unloaded range.
 *
 * Logical indices are oldest -> newest for both channel and thread timelines.
 */
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

    explicit PostTimeline(int initialEstimatedRowHeight = 96);

    void reset(int totalCount = 0);
    void setTotalCount(int totalCount);
    int totalCount() const { return logicalCount; }
    int loadedCount() const { return loadedByIndex.size(); }

    /**
     * Place an authoritative chronological window at exact logical indices.
     * Existing occurrences of the same post IDs are relocated, so an ID can
     * never exist at two positions in the same timeline.
     */
    void placeWindow(int firstIndex, const QStringList& chronologicalPostIds);

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
