/**
 * @file ThreadTimelineSeek.cpp
 * @brief Shared-policy staged random seek for sparse thread timelines.
 *
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#include "ThreadTimelineController.h"

#include <algorithm>
#include <utility>

#include <QPointer>
#include <QTimer>

#include "ChatArea.h"
#include "PostsListWidget.h"
#include "backend/PostTimelineService.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
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

QString ThreadTimelineController::seekFocusPostId(
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

void ThreadTimelineController::renderSeekWindow(
    const TimelineSeekState::Ticket& ticket)
{
    if (!seekState.isCurrent(ticket)) {
        return;
    }

    // Wheel/local prefetch owns one concrete viewport anchor for the whole
    // generation. A thumb random seek deliberately leaves this empty so the
    // logical target is centred after materialization instead of preserving an
    // estimated gap pixel that may no longer map to the same content.
    if (seekPreserveViewport && seekViewportAnchor.isValid()) {
        renderTimeline(QString(), false, seekViewportAnchor);
        return;
    }
    renderTimeline(seekFocusPostId(ticket), false, ViewportAnchor());
}

void ThreadTimelineController::requestSeekSeed(const TimelineSeekState::Ticket& ticket)
{
    if (!seekState.isCurrent(ticket) || requestInFlight
        || ticket.targetIndex <= 0 || ticket.targetIndex >= timeline.totalCount()) {
        return;
    }

    seekState.begin(ticket);
    pendingSeekIndex = ticket.targetIndex;
    // Do not recapture here. updateSeekTargetFromScrollbar() has already chosen
    // centre-target semantics for a thumb drag, while checkViewport() has already
    // stored a concrete anchor for slow wheel/local prefetch.

    if (!timeline.postIdAt(ticket.targetIndex).isEmpty()) {
        renderSeekWindow(ticket);
        requestSeekExpansion(ticket);
        return;
    }

    BackendPost* root = area.channel.postIdToPost.value(rootId, nullptr);
    if (!root || root->last_reply_at <= root->create_at || expectedPostCount <= 1) {
        finishSeek(ticket);
        requestNextPage();
        return;
    }

    const long double fraction = static_cast<long double>(ticket.targetIndex)
        / static_cast<long double>(std::max(1, expectedPostCount - 1));
    const long double span = static_cast<long double>(root->last_reply_at - root->create_at);
    const uint64_t estimatedCreateAt = root->create_at
        + static_cast<uint64_t>(span * fraction);

    requestInFlight = true;
    QPointer<ThreadTimelineController> guard(this);
    PostTimelineService::instance(area.backend).loadThreadFromTime(
        area.channel, rootId, SeekSeedSize, estimatedCreateAt,
        [guard, ticket](const PostTimelineService::Page& page) {
            if (!guard) {
                return;
            }
            guard->requestInFlight = false;

            // Successful stale responses have already been merged into the
            // BackendChannel memory model. They are reusable data, just not a
            // command to move a viewport whose thumb has since moved elsewhere.
            if (!guard->seekState.isCurrent(ticket)) {
                guard->resumeSeekIfReady();
                return;
            }
            if (!page.success || page.postIds.isEmpty()) {
                guard->finishSeek(ticket);
                return;
            }

            QStringList ids = page.postIds;
            ids.removeAll(guard->rootId);
            if (ids.isEmpty()) {
                guard->finishSeek(ticket);
                return;
            }

            PostTimeline::LogicalWindow window = guard->timeline.gapWindowNear(
                ticket.targetIndex, static_cast<int>(ids.size()), 1);
            if (!window.isValid()) {
                guard->finishSeek(ticket);
                return;
            }
            if (ids.size() > window.count) {
                ids = ids.mid(0, window.count);
            }
            guard->timeline.placeWindow(window.firstIndex, ids);

            guard->renderSeekWindow(ticket);
            guard->persistState();
            guard->requestSeekExpansion(ticket);
        });
}

void ThreadTimelineController::requestSeekExpansion(
    const TimelineSeekState::Ticket& ticket,
    bool afterMeasurement)
{
    if (!seekState.isActive(ticket) || requestInFlight
        || !area.ui || !area.ui->listWidget) {
        return;
    }

    const PostTimeline::LogicalWindow loaded = timeline.loadedWindowContaining(
        ticket.targetIndex);
    if (!loaded.isValid()) {
        finishSeek(ticket);
        return;
    }

    // Logical row 0 is the root and there can be no older reply before row 1.
    if (loaded.firstIndex <= 1) {
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
        QPointer<ThreadTimelineController> guard(this);
        QTimer::singleShot(SeekMeasurementDelayMs, this, [guard, ticket] {
            if (guard && guard->seekState.isActive(ticket)) {
                guard->requestSeekExpansion(ticket, true);
            }
        });
        return;
    }

    finishSeek(ticket);
}

void ThreadTimelineController::requestSeekEdge(
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
    BackendPost* cursorPost = area.channel.postIdToPost.value(cursorId, nullptr);
    if (cursorId.isEmpty() || !cursorPost) {
        seekState.markBoundary(ticket, edge);
        requestSeekExpansion(ticket);
        return;
    }

    requestInFlight = true;
    QPointer<ThreadTimelineController> guard(this);
    auto callback = [guard, ticket, edge, cursorId](const PostTimelineService::Page& page) {
        if (!guard) {
            return;
        }
        guard->requestInFlight = false;
        if (!guard->seekState.isCurrent(ticket)) {
            guard->resumeSeekIfReady();
            return;
        }
        if (!page.success) {
            guard->finishSeek(ticket);
            return;
        }

        QStringList ids = page.postIds;
        ids.removeAll(guard->rootId);
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
            const int available = std::max(0, current.firstIndex - 1); // row 0 is root
            if (missing.size() > available) {
                missing = missing.mid(missing.size() - available);
            }
            if (!missing.isEmpty()) {
                guard->timeline.placeWindow(current.firstIndex - missing.size(), missing);
            }
            if (ids.size() < SeekEdgeSize || current.firstIndex <= 1) {
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
            if (ids.size() < SeekEdgeSize
                || current.lastIndex() >= guard->timeline.totalCount() - 1) {
                guard->seekState.markBoundary(ticket, edge);
            }
        }

        if (missing.isEmpty()) {
            guard->seekState.markBoundary(ticket, edge);
        }

        guard->renderSeekWindow(ticket);
        guard->persistState();
        guard->requestSeekExpansion(ticket);
    };

    PostTimelineService& service = PostTimelineService::instance(area.backend);
    if (older) {
        service.loadThreadBefore(area.channel, rootId, cursorId,
                                 cursorPost->create_at, SeekEdgeSize,
                                 std::move(callback));
    } else {
        service.loadThreadAfter(area.channel, rootId, cursorId,
                                cursorPost->create_at, SeekEdgeSize,
                                std::move(callback));
    }
}

void ThreadTimelineController::finishSeek(const TimelineSeekState::Ticket& ticket)
{
    if (!seekState.isCurrent(ticket)) {
        return;
    }
    seekState.complete(ticket);
    pendingSeekIndex = -1;
    seekPreserveViewport = false;
    seekViewportAnchor = ViewportAnchor();
    persistState();
    schedulePrune();

    // A completed bounded seek is terminal. New wheel/drag input schedules the
    // next viewport check. Stale callbacks still resume a newer ready ticket
    // directly, so no autonomous finish -> check -> seek loop is required.
}

} // namespace Mattermost
