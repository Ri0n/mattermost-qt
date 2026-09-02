/**
 * @file ThreadTimelineController.h
 * @brief Lazy/prefetched loading for large thread windows.
 *
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#pragma once

#include <cstdint>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

#include "backend/PostTimeline.h"

namespace Mattermost {

class ChatArea;
class ThreadTimelineController;

ThreadTimelineController* createThreadTimelineController(ChatArea& area);

class ThreadTimelineController : public QObject
{
    Q_OBJECT
public:
    explicit ThreadTimelineController(ChatArea& area);

private:
    void start();
    void requestNextPage();
    void requestSeek(int logicalIndex);
    void scheduleViewportCheck();
    void checkViewport();
    int logicalIndexForNearbyGap(bool* viewportCenterInsideGap) const;

    void renderTimeline(const QString& focusPostId = QString(), bool focusAtTop = false);
    void updateGapHeights();
    void scheduleMeasurementPass();
    void measureRenderedPosts();
    void schedulePaintResume(quint64 renderId);

    ChatArea& area;
    QString rootId;
    QString cursorPostId;
    uint64_t cursorCreateAt = 0;
    PostTimeline timeline;
    QTimer seekTimer;
    QTimer measurementTimer;
    int expectedPostCount = 1;
    int nextLogicalIndex = 0;
    int initialPagesRemaining = 0;
    int pendingSeekIndex = -1;
    int lastAppliedGapRowHeight = 96;
    quint64 renderGeneration = 0;
    bool initialPrefetchDone = false;
    bool requestInFlight = false;
    bool hasNext = true;
    bool viewportCheckScheduled = false;
    bool rebuilding = false;
};

} // namespace Mattermost
