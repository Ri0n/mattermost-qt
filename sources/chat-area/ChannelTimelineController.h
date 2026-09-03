/**
 * @file ChannelTimelineController.h
 * @brief Sparse/windowed loading for normal channel timelines.
 *
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#pragma once

#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>

#include "backend/PostTimeline.h"

class QEvent;

namespace Mattermost {

class BackendPost;
class ChatArea;
struct ChannelNewPosts;
class ChannelTimelineController;

ChannelTimelineController* createChannelTimelineController(ChatArea& area);

class ChannelTimelineController : public QObject
{
    Q_OBJECT
public:
    explicit ChannelTimelineController(ChatArea& area);

    bool ensurePostVisible(const QString& postId);
    bool ensurePinnedPostVisible(const QString& postId,
                                 const QStringList& contextPostIds,
                                 bool reachedOldest,
                                 bool reachedNewest);
    void beginContextNavigation(const QString& postId);
    void requestOlderPage();
    void materializeLivePost(BackendPost& post);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

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
        int distanceFromNewest = -1;
        int offsetWithinEstimatedRow = 0;

        bool isValid() const { return kind != None; }
    };

    void tryStart();
    void start();
    void deactivate();

    void requestPage(int page, int focusLogicalIndex = -1);
    void requestContextBefore();
    void requestContextAfter();
    void continueInitialPrefetch();
    void requestPageForIndex(int logicalIndex, bool focusAfterLoad);

    void absorbNewPosts(const ChannelNewPosts& newPosts);
    void flushDeferredExternalPosts(bool renderAfter = true);
    void placeApproximateWindow(const QStringList& postIds,
                                const QString& focusPostId = QString(),
                                bool renderAfter = true);
    int estimateLogicalIndex(uint64_t createAt) const;
    int gapPlacementForWindow(int estimatedCenter, int count) const;

    ViewportAnchor captureViewportAnchor() const;
    ViewportAnchor stableViewportAnchor() const;
    void restoreViewportAnchor(const ViewportAnchor& anchor,
                               const QString& focusPostId = QString());

    void scheduleViewportCheck();
    void checkViewport();
    void requestSeek(int logicalIndex);
    int logicalIndexNearViewport(int extraScreens, bool* centerInsideGap) const;

    void renderTimeline();
    void renderTimeline(const QString& focusPostId);
    void renderTimeline(const QString& focusPostId, const ViewportAnchor& anchor);
    void updateGapHeights();
    void scheduleMeasurementPass();
    void measureRenderedPosts();
    void schedulePaintResume(quint64 renderId);

    void schedulePrune();
    void pruneLoadedPosts(quint64 pruneRequestGeneration);
    int logicalIndexForAnchor(const ViewportAnchor& anchor) const;

    int authoritativeFirstIndex(int page, int pageSize) const;
    int channelRootPostCount() const;

    ChatArea& area;
    PostTimeline timeline;
    QTimer seekTimer;
    QTimer measurementTimer;
    QSet<int> loadedPages;
    QStringList deferredExternalPostIds;

    ViewportAnchor lastUserViewportAnchor;
    QString contextNavigationPostId;
    QString contextOldestPostId;
    QString contextNewestPostId;

    int expectedPostCount = 0;
    int nextOlderPage = 0;
    int initialPageTarget = 0;
    int pendingSeekIndex = -1;
    int requestedPage = -1;
    int requestedFocusIndex = -1;
    int lastAppliedGapRowHeight = 96;

    quint64 generation = 0;
    quint64 renderGeneration = 0;
    quint64 pruneGeneration = 0;
    bool active = false;
    bool totalCountExact = false;
    bool initialRenderDone = false;
    bool requestInFlight = false;
    bool viewportCheckScheduled = false;
    bool rebuilding = false;
    bool contextNavigationActive = false;
    bool contextReachedOldest = false;
    bool contextReachedNewest = false;
};

} // namespace Mattermost
