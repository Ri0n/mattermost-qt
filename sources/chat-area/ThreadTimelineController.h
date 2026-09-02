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

#include "backend/PostTimeline.h"

namespace Mattermost {

class ChatArea;
class ThreadTimelineController;

/** Created from ChatArea's late member initializer; returns null for channels. */
ThreadTimelineController* createThreadTimelineController(ChatArea& area);

class ThreadTimelineController : public QObject
{
    Q_OBJECT
public:
    explicit ThreadTimelineController(ChatArea& area);

private:
    void start();
    void requestNextPage();
    void scheduleViewportCheck();
    void checkViewport();

    ChatArea& area;
    QString rootId;
    QString cursorPostId;
    uint64_t cursorCreateAt = 0;
    PostTimeline timeline;
    int expectedPostCount = 1;
    int nextLogicalIndex = 0;
    int initialPagesRemaining = 0;
    bool requestInFlight = false;
    bool hasNext = true;
    bool viewportCheckScheduled = false;
};

} // namespace Mattermost
