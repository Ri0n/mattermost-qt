/**
 * @file ChannelTimelineSeek.cpp
 * @brief Debounced random seek for sparse channel timelines.
 *
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#include "ChannelTimelineController.h"

#include <algorithm>

#include <QPointer>
#include <QScrollBar>

#include "ChatArea.h"
#include "PostsListWidget.h"
#include "backend/PostTimelineService.h"
#include "ui_ChatArea.h"

namespace Mattermost {
namespace {

constexpr int SeekSeedSize = 10;

} // namespace

void ChannelTimelineController::updateSeekTargetFromScrollbar(bool readyImmediately)
{
    if (!active || !area.ui || !area.ui->listWidget || timeline.totalCount() <= 0) {
        return;
    }

    QScrollBar* bar = area.ui->listWidget->verticalScrollBar();
    const int target = timeline.logicalIndexForScrollPosition(
        bar->value(), bar->minimum(), bar->maximum());
    if (target < 0) {
        return;
    }

    if (seekState.setTarget(target)) {
        pendingSeekIndex = target;
    }

    if (readyImmediately) {
        seekTimer.stop();
        seekState.markReady();
        resumeSeekIfReady();
    } else {
        seekTimer.start();
    }
}

void ChannelTimelineController::resumeSeekIfReady()
{
    if (!seekState.isReady() || requestInFlight || !active) {
        return;
    }
    requestSeek(seekState.currentTicket());
}

void ChannelTimelineController::requestSeek(const TimelineSeekState::Ticket& ticket)
{
    if (!active || !seekState.isCurrent(ticket) || requestInFlight
        || ticket.targetIndex < 0 || ticket.targetIndex >= timeline.totalCount()) {
        return;
    }

    pendingSeekIndex = ticket.targetIndex;
    if (!timeline.postIdAt(ticket.targetIndex).isEmpty()) {
        const QString targetId = timeline.postIdAt(ticket.targetIndex);
        seekState.complete(ticket);
        pendingSeekIndex = -1;
        renderTimeline(targetId, ViewportAnchor());
        return;
    }

    const int total = timeline.totalCount();
    const int distanceFromNewest = total - 1 - ticket.targetIndex;
    const int page = std::max(0, distanceFromNewest / SeekSeedSize);

    requestInFlight = true;
    QPointer<ChannelTimelineController> guard(this);
    PostTimelineService::instance(area.backend).loadChannelPage(
        area.channel, page, SeekSeedSize,
        [guard, ticket, page](const PostTimelineService::Page& result) {
            if (!guard) {
                return;
            }
            guard->requestInFlight = false;

            // The service already ingested every successful response into the
            // BackendChannel memory cache. A seek response that lost the race
            // with continued thumb movement is therefore retained for reuse but
            // must not mutate the visible sparse topology.
            if (!guard->seekState.isCurrent(ticket)) {
                guard->resumeSeekIfReady();
                return;
            }
            if (!result.success || result.postIds.isEmpty()) {
                guard->seekState.complete(ticket);
                guard->pendingSeekIndex = -1;
                return;
            }

            const QStringList ids = result.postIds;
            const int responseSize = static_cast<int>(ids.size());
            const int firstIndex = std::max(0,
                guard->timeline.totalCount() - page * SeekSeedSize - responseSize);
            guard->timeline.placeWindow(firstIndex, ids);

            int focusIndex = std::max(firstIndex,
                std::min(firstIndex + responseSize - 1, ticket.targetIndex));
            QString focusId = guard->timeline.postIdAt(focusIndex);
            if (focusId.isEmpty()) {
                focusId = ids.at(ids.size() / 2);
            }

            guard->seekState.complete(ticket);
            guard->pendingSeekIndex = -1;
            guard->renderTimeline(focusId, ViewportAnchor());
            guard->schedulePrune();
        });
}

} // namespace Mattermost
