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
#include <utility>

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

    // A physical thumb drag is a logical random seek: after materialization the
    // requested logical row belongs near the viewport centre. Do not preserve a
    // pre-seek geometry-only gap anchor. Slow wheel prefetch takes the other path
    // through userViewportChanged/checkViewport and retains its concrete anchor.
    seekPreserveViewport = false;
    seekViewportAnchor = ViewportAnchor();
    lastUserViewportAnchor = ViewportAnchor();

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

void ChannelTimelineController::renderSeekWindow(
    const TimelineSeekState::Ticket& ticket)
{
    if (!active || !seekState.isCurrent(ticket)) {
        return;
    }

    // userViewportChanged is emitted only for real input. If it changed after
    // this generation captured its wheel anchor, supersede the generation rather
    // than allowing a late HTTP response to pull the viewport back. Thumb seek
    // deliberately has no preserved anchor and therefore centres its target.
    if (seekPreserveViewport
        && seekViewportAnchor.isValid() && lastUserViewportAnchor.isValid()) {
        const int originalIndex = logicalIndexForAnchor(seekViewportAnchor);
        const int currentUserIndex = logicalIndexForAnchor(lastUserViewportAnchor);
        if (originalIndex >= 0 && currentUserIndex >= 0
            && currentUserIndex != originalIndex) {
            seekState.setTarget(currentUserIndex);
            pendingSeekIndex = currentUserIndex;
            seekViewportAnchor = lastUserViewportAnchor;
            seekPreserveViewport = true;
            seekState.markReady();
            resumeSeekIfReady();
            return;
        }
    }

    if (seekPreserveViewport && seekViewportAnchor.isValid()) {
        renderTimeline(QString(), seekViewportAnchor);
        return;
    }
    renderTimeline(seekFocusPostId(ticket), ViewportAnchor());
}

void ChannelTimelineController::requestSeek(const TimelineSeekState::Ticket& ticket)
{
    if (!active || !seekState.isCurrent(ticket) || requestInFlight
        || ticket.targetIndex < 0 || ticket.targetIndex >= timeline.totalCount()) {
        return;
    }

    seekState.begin(ticket);
    pendingSeekIndex = ticket.targetIndex;

    // Slow wheel/local prefetch reaches us after userViewportChanged committed a
    // concrete post/gap anchor. Preserve that one anchor for the whole staged
    // transaction. Thumb seek cleared lastUserViewportAnchor above, so it instead
    // centres the target after the seed arrives.
    if (lastUserViewportAnchor.isValid()) {
        seekViewportAnchor = lastUserViewportAnchor;
        seekPreserveViewport = true;
    } else {
        seekViewportAnchor = ViewportAnchor();
        seekPreserveViewport = false;
    }

    if (!timeline.postIdAt(ticket.targetIndex).isEmpty()) {
        renderSeekWindow(ticket);
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

            guard->renderSeekWindow(ticket);
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

    if (loaded.firstIndex <= 0) {
        seekState.markBoundary(ticket, TimelineSeekState::OlderEdge);
    }
    if (loaded.lastIndex() >= timeline.totalCount() - 1) {
        seekState.markBoundary(ticket, TimelineSeekState::NewerEdge);
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

        guard->renderSeekWindow(ticket);
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
    seekPreserveViewport = false;
    seekViewportAnchor = ViewportAnchor();
    schedulePrune();

    // A user wheel/drag may have moved again while the final expansion request
    // was in flight. Re-evaluate the actual viewport once the old generation no
    // longer owns networking; if it is already covered this is a cheap no-op.
    scheduleViewportCheck();
}

} // namespace Mattermost