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

class QListWidgetItem;
class QEvent;

namespace Mattermost {

class ChatArea;
class ChannelNewPosts;
class ChannelTimelineController;

/** Created from ChatArea's late member initializer; returns null for threads. */
ChannelTimelineController* createChannelTimelineController(ChatArea& area);

class ChannelTimelineController : public QObject
{
    Q_OBJECT
public:
    explicit ChannelTimelineController(ChatArea& area);

    /** Materialize a cached context window around postId into the sparse view. */
    bool ensurePostVisible(const QString& postId);

    /** Load the next older sequential page. Used by the legacy button/top signal. */
    void requestOlderPage();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void tryStart();
    void start();
    void deactivate();

    void requestPage(int page, int focusLogicalIndex = -1);
    void continueInitialPrefetch();
    void requestPageForIndex(int logicalIndex, bool focusAfterLoad);

    void absorbNewPosts(const ChannelNewPosts& newPosts);
    void flushDeferredExternalPosts();
    void placeApproximateWindow(const QStringList& postIds,
                                const QString& focusPostId = QString());
    int estimateLogicalIndex(uint64_t createAt) const;
    int gapPlacementForWindow(int estimatedCenter, int count) const;

    void scheduleViewportCheck();
    void checkViewport();
    void requestSeek(int logicalIndex);

    void renderTimeline(const QString& focusPostId = QString());
    void updateGapHeights();
    void scheduleMeasuredHeightUpdate(const QString& postId);
    QListWidgetItem* gapAtViewportCenter() const;
    int logicalIndexInsideGap(const QListWidgetItem* gapItem) const;
    int nearbyGapLogicalIndex() const;

    int authoritativeFirstIndex(int page, int pageSize) const;
    int channelRootPostCount() const;

    ChatArea& area;
    PostTimeline timeline;
    QTimer seekTimer;
    QSet<int> loadedPages;
    QStringList deferredExternalPostIds;

    int expectedPostCount = 0;
    int nextOlderPage = 0;
    int initialPageTarget = 0;
    int pendingSeekIndex = -1;
    int requestedPage = -1;
    int requestedFocusIndex = -1;

    quint64 generation = 0;
    bool active = false;
    bool requestInFlight = false;
    bool viewportCheckScheduled = false;
    bool deferredExternalFlushScheduled = false;
    bool rebuilding = false;
};

} // namespace Mattermost
