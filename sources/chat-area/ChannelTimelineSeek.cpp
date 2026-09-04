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
#include <QTimer>

#include "ChatArea.h"
#include "PostsListWidget.h"
#include "backend/PostTimelineService.h"
#include "ui_ChatArea.h"

namespace Mattermost {
namespace {

constexpr int SeekSeedSize = 10;
constexpr int SeekEdgeSize = 10;
constexpr int MinimumSeekWindow = 30;
constexpr int MaximumSeekWindow = 90;
constexpr int MaximumSeekExpansionRequests = 8;
constexpr int SeekMeasurementDelayMs = 220;

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

QString ChannelTimelineController::seekFocusPostId(
    const TimelineSeekState::Ticket& ticket) const
{
    if (!seekState.isCurrent(ticket)) {
        return {};
    }
    QString id = timeline.postIdAt(ticket.targetIndex);
    if (!id.isEmpty()) {
        return id;
    }
    const PostTimeline::LogicalWindow loaded = timeline.loadedWindowContaining(ticket.targetIndex);
    if (!loaded.isValid()) {
        return {};
    }
    const int nearest = std::max(loaded.firstIndex,
        std::min(loaded.lastIndex(), ticket.targetIndex));
    return timeline.postIdAt(nearest);
}

void ChannelTimelineController::requestSeek(const TimelineSeekState::Ticket& ticket)
{
    if (!active || !seekState.isCurrent(ticket) || requestInFlight
        || ticket.targetIndex < 0 || ticket.targetIndex >= timeline.totalCount()) {
        return;
    }

    seekState.begin(ticket);
    pendingSeekIndex = ticket.targetIndex;

    if (!timeline.postIdAt(ticket.targetIndex).isEmpty()) {
        renderTimeline(timeline.postIdAt(ticket.targetIndex), ViewportAnchor());
        requestSeekExpansion(ticket);
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

            // Every successful service response is ingested before this callback.
            // Stale drag requests therefore remain useful in the memory model but
            // are forbidden from changing the visible sparse topology.
            if (!guard->seekState.isCurrent(ticket)) {
                guard->resumeSeekIfReady();
                return;
            }
            if (!result.success || result.postIds.isEmpty()) {
                guard->finishSeek(ticket);
                return;
            }

            const QStringList ids = result.postIds;
            const int responseSize = static_cast<int>(ids.size());
            const int firstIndex = std::max(0,
                guard->timeline.totalCount() - page * SeekSeedSize - responseSize);
            guard->timeline.placeWindow(firstIndex, ids);

            const QString focusId = guard->seekFocusPostId(ticket);
            guard->renderTimeline(focusId, ViewportAnchor());
            guard->requestSeekExpansion(ticket);
        });
}

void ChannelTimelineController::requestSeekExpansion(
    const TimelineSeekState::Ticket& ticket,
    bool afterMeasurement)
{
    if (!active || !seekState.isActive(ticket) || requestInFlight
        || !area.ui || !area.ui->listWidget) {
        return;
    }

    const PostTimeline::LogicalWindow loaded = timeline.loadedWindowContaining(
        ticket.targetIndex);
    if (!loaded.isValid()) {
        finishSeek(ticket);
        return;
    }

    const int requiredRows = afterMeasurement
        ? timeline.rowsForViewportCoverage(
              area.ui->listWidget->viewport()->height(), 1,
              MinimumSeekWindow, MaximumSeekWindow)
        : MinimumSeekWindow;

    const TimelineSeekState::Edge edge = seekState.nextEdge(
        ticket, loaded.firstIndex, loaded.lastIndex(), requiredRows);
    if (edge != TimelineSeekState::NoEdge) {
        if (!seekState.noteExpansionRequest(ticket, MaximumSeekExpansionRequests)) {
            finishSeek(ticket);
            return;
        }
        requestSeekEdge(ticket, edge);
        return;
    }

    if (!afterMeasurement) {
        // The first 10+10+10 transaction is intentionally fast. Let actual row
        // geometry settle once, then decide whether tiny posts require another
        // bounded 10-row edge extension to cover viewport + one screen each side.
        QPointer<ChannelTimelineController> guard(this);
        QTimer::singleShot(SeekMeasurementDelayMs, this, [guard, ticket] {
            if (guard && guard->seekState.isActive(ticket)) {
                guard->requestSeekExpansion(ticket, true);
            }
        });
        return;
    }

    finishSeek(ticket);
}

void ChannelTimelineController::requestSeekEdge(
    const TimelineSeekState::Ticket& ticket,
    TimelineSeekState::Edge edge)
{
    if (!seekState.isActive(ticket) || requestInFlight) {
        return;
    }

    const PostTimeline::LogicalWindow loaded = timeline.loadedWindowContaining(
        ticket.targetIndex);
    if (!loaded.isValid()) {
        finishSeek(ticket);
        return;
    }

    const bool older = edge == TimelineSeekState::OlderEdge;
    const int cursorIndex = older ? loaded.firstIndex : loaded.lastIndex();
    const QString cursorId = timeline.postIdAt(cursorIndex);
    if (cursorId.isEmpty()) {
        seekState.markBoundary(ticket, edge);
        requestSeekExpansion(ticket);
        return;
    }

    requestInFlight = true;
    QPointer<ChannelTimelineController> guard(this);
    auto callback = [guard, ticket, edge, cursorId](const PostTimelineService::Page& result) {
        if (!guard) {
            return;
        }
        guard->requestInFlight = false;
        if (!guard->seekState.isCurrent(ticket)) {
            guard->resumeSeekIfReady();
            return;
        }
        if (!result.success) {
            guard->finishSeek(ticket);
            return;
        }

        QStringList ids = result.postIds;
        ids.removeAll(cursorId);
        QStringList missing;
        for (const QString& id : ids) {
            if (!id.isEmpty() && !guard->timeline.contains(id)) {
                missing.push_back(id);
            }
        }

        const PostTimeline::LogicalWindow current = guard->timeline.loadedWindowContaining(
            ticket.targetIndex);
        if (!current.isValid()) {
            guard->finishSeek(ticket);
            return;
        }

        if (edge == TimelineSeekState::OlderEdge) {
            const int available = current.firstIndex;
            if (missing.size() > available) {
                missing = missing.mid(missing.size() - available);
            }
            if (!missing.isEmpty()) {
                guard->timeline.placeWindow(current.firstIndex - missing.size(), missing);
            }
            if (result.prevPostId.isEmpty() || ids.size() < SeekEdgeSize) {
                guard->seekState.markBoundary(ticket, edge);
            }
        } else {
            const int available = guard->timeline.totalCount() - 1 - current.lastIndex();
            if (missing.size() > available) {
                missing = missing.mid(0, available);
            }
            if (!missing.isEmpty()) {
                guard->timeline.placeWindow(current.lastIndex() + 1, missing);
            }
            if (result.nextPostId.isEmpty() || ids.size() < SeekEdgeSize) {
                guard->seekState.markBoundary(ticket, edge);
            }
        }

        if (missing.isEmpty()) {
            guard->seekState.markBoundary(ticket, edge);
        }

        guard->renderTimeline(guard->seekFocusPostId(ticket), ViewportAnchor());
        guard->requestSeekExpansion(ticket);
    };

    PostTimelineService& service = PostTimelineService::instance(area.backend);
    if (older) {
        service.loadChannelBefore(area.channel, cursorId, SeekEdgeSize, std::move(callback));
    } else {
        service.loadChannelAfter(area.channel, cursorId, SeekEdgeSize, std::move(callback));
    }
}

void ChannelTimelineController::finishSeek(const TimelineSeekState::Ticket& ticket)
{
    if (!seekState.isCurrent(ticket)) {
        return;
    }
    seekState.complete(ticket);
    pendingSeekIndex = -1;
    schedulePrune();
}

} // namespace Mattermost
