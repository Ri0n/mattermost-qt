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
#include <QString>
#include <QTimer>

#include "backend/PostTimeline.h"
#include "backend/TimelineSeekState.h"

namespace Mattermost {

class BackendPost;
class ChatArea;
class ThreadTimelineController;

ThreadTimelineController* createThreadTimelineController(ChatArea& area);
ThreadTimelineController* createConfiguredThreadTimelineController(ChatArea& area);

class ThreadTimelineController : public QObject
{
    Q_OBJECT
public:
    explicit ThreadTimelineController(ChatArea& area);
    bool ensurePostVisible(const QString& postId);
    void installIncrementalLiveUpdates();
    void materializeLivePost(BackendPost& post);
    void openNewestOnInitialOpen();

private:
    struct ViewportAnchor {
        enum Kind {
            None,
            Bottom,
            Post,
            Gap,
        } kind = None;

        QString postId;
        int postTopOffset = 0;
        int logicalIndex = -1;
        int offsetWithinEstimatedRow = 0;

        bool isValid() const { return kind != None; }
    };

    void start();
    void requestNextPage();
    void updateSeekTargetFromScrollbar(bool readyImmediately);
    void resumeSeekIfReady();
    void requestSeek(const TimelineSeekState::Ticket& ticket);
    void requestSeekSeed(const TimelineSeekState::Ticket& ticket);
    void requestSeekExpansion(const TimelineSeekState::Ticket& ticket,
                              bool afterMeasurement = false);
    void requestSeekEdge(const TimelineSeekState::Ticket& ticket,
                         TimelineSeekState::Edge edge);
    void finishSeek(const TimelineSeekState::Ticket& ticket);
    QString seekFocusPostId(const TimelineSeekState::Ticket& ticket) const;
    void scheduleViewportCheck();
    void checkViewport();
    int logicalIndexNearViewport(int extraScreens, bool* viewportCenterInsideGap) const;

    ViewportAnchor captureViewportAnchor() const;
    void restoreViewportAnchor(const ViewportAnchor& anchor,
                               const QString& focusPostId = QString(),
                               bool focusAtTop = false);
    bool restoreSavedState(ViewportAnchor& anchor);
    void persistState();

    void renderTimeline(const QString& focusPostId = QString(), bool focusAtTop = false);
    void renderTimeline(const QString& focusPostId,
                        bool focusAtTop,
                        const ViewportAnchor& anchor);
    void updateGapHeights();
    void scheduleMeasurementPass();
    void measureRenderedPosts();
    void schedulePaintResume(quint64 renderId);

    void schedulePrune();
    void pruneLoadedPosts(quint64 pruneRequestGeneration);
    int logicalIndexForAnchor(const ViewportAnchor& anchor) const;

    ChatArea& area;
    QString rootId;
    QString cursorPostId;
    uint64_t cursorCreateAt = 0;
    PostTimeline timeline;
    TimelineSeekState seekState;
    QTimer seekTimer;
    QTimer measurementTimer;
    int expectedPostCount = 1;
    int nextLogicalIndex = 0;
    int initialPagesRemaining = 0;
    int pendingSeekIndex = -1;
    int lastAppliedGapRowHeight = 96;
    quint64 renderGeneration = 0;
    quint64 pruneGeneration = 0;
    bool initialPrefetchDone = false;
    bool requestInFlight = false;
    bool hasNext = true;
    bool viewportCheckScheduled = false;
    bool rebuilding = false;
    bool initialNewestRequestInFlight = false;
    bool initialNewestOpenDone = false;
};

} // namespace Mattermost
