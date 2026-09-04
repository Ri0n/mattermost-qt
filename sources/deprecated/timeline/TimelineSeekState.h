/**
 * @file TimelineSeekState.h
 * @brief Shared state machine for debounced sparse timeline seeks.
 *
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <algorithm>

#include <QtGlobal>

namespace Mattermost {

class TimelineSeekState
{
public:
    enum Edge {
        NoEdge,
        OlderEdge,
        NewerEdge,
    };

    struct Ticket {
        quint64 generation = 0;
        int targetIndex = -1;

        bool isValid() const { return targetIndex >= 0; }
    };

    void reset()
    {
        ++generation;
        targetIndex = -1;
        ready = false;
        active = false;
        olderBoundary = false;
        newerBoundary = false;
        expansionRequests = 0;
    }

    bool setTarget(int index)
    {
        if (index == targetIndex) {
            return false;
        }
        ++generation;
        targetIndex = index;
        ready = false;
        active = false;
        olderBoundary = false;
        newerBoundary = false;
        expansionRequests = 0;
        return true;
    }

    void markReady()
    {
        ready = targetIndex >= 0;
    }

    void begin(const Ticket& ticket)
    {
        if (!isCurrent(ticket)) {
            return;
        }
        ready = false;
        active = true;
        olderBoundary = false;
        newerBoundary = false;
        expansionRequests = 0;
    }

    void complete(const Ticket& ticket)
    {
        if (isCurrent(ticket)) {
            ready = false;
            active = false;
        }
    }

    bool isReady() const { return ready && targetIndex >= 0; }
    bool hasActiveTransaction() const { return active && targetIndex >= 0; }
    bool isActive(const Ticket& ticket) const
    {
        return active && isCurrent(ticket);
    }
    int target() const { return targetIndex; }

    Ticket currentTicket() const
    {
        return Ticket {generation, targetIndex};
    }

    bool isCurrent(const Ticket& ticket) const
    {
        return ticket.isValid()
            && ticket.generation == generation
            && ticket.targetIndex == targetIndex;
    }

    void markBoundary(const Ticket& ticket, Edge edge)
    {
        if (!isCurrent(ticket)) {
            return;
        }
        if (edge == OlderEdge) {
            olderBoundary = true;
        } else if (edge == NewerEdge) {
            newerBoundary = true;
        }
    }

    /**
     * Grow a contiguous window until TARGET has useful coverage on both sides,
     * not merely until the span contains enough rows in total. An absolute
     * server page can place TARGET at one edge of the first 10-row seed; stopping
     * at 30 total rows in that state leaves the viewport visibly biased and can
     * immediately expose another gap.
     *
     * Real oldest/newest boundaries relax the corresponding side requirement.
     */
    Edge nextEdge(const Ticket& ticket,
                  int loadedFirst,
                  int loadedLast,
                  int requiredRows) const
    {
        if (!isActive(ticket) || loadedFirst < 0 || loadedLast < loadedFirst
            || requiredRows <= 0) {
            return NoEdge;
        }

        const int target = std::max(loadedFirst,
            std::min(loadedLast, ticket.targetIndex));
        const int before = target - loadedFirst;
        const int after = loadedLast - target;

        const int desiredBefore = (requiredRows - 1) / 2;
        const int desiredAfter = requiredRows - 1 - desiredBefore;
        const bool needOlder = !olderBoundary && before < desiredBefore;
        const bool needNewer = !newerBoundary && after < desiredAfter;

        if (!needOlder && !needNewer) {
            return NoEdge;
        }
        if (needOlder && !needNewer) {
            return OlderEdge;
        }
        if (!needOlder && needNewer) {
            return NewerEdge;
        }

        // Both sides are short. Prefer the side with the larger absolute deficit;
        // ties go older first, producing the intended seed + older + newer staged
        // order without encoding channel/thread endpoint details here.
        const int olderDeficit = desiredBefore - before;
        const int newerDeficit = desiredAfter - after;
        return olderDeficit >= newerDeficit ? OlderEdge : NewerEdge;
    }

    bool noteExpansionRequest(const Ticket& ticket, int maximumRequests)
    {
        if (!isActive(ticket) || expansionRequests >= std::max(0, maximumRequests)) {
            return false;
        }
        ++expansionRequests;
        return true;
    }

private:
    quint64 generation = 0;
    int targetIndex = -1;
    int expansionRequests = 0;
    bool ready = false;
    bool active = false;
    bool olderBoundary = false;
    bool newerBoundary = false;
};

} // namespace Mattermost
