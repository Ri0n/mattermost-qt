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
     * Pick the less-covered side of the current contiguous loaded window. This
     * keeps the target approximately centred while growing the window and never
     * retries an edge already proven to be a real server boundary.
     */
    Edge nextEdge(const Ticket& ticket,
                  int loadedFirst,
                  int loadedLast,
                  int requiredRows) const
    {
        if (!isActive(ticket) || loadedFirst < 0 || loadedLast < loadedFirst
            || requiredRows <= loadedLast - loadedFirst + 1) {
            return NoEdge;
        }

        const int target = std::max(loadedFirst,
            std::min(loadedLast, ticket.targetIndex));
        const int before = target - loadedFirst;
        const int after = loadedLast - target;

        if (!olderBoundary && !newerBoundary) {
            return before <= after ? OlderEdge : NewerEdge;
        }
        if (!olderBoundary) {
            return OlderEdge;
        }
        if (!newerBoundary) {
            return NewerEdge;
        }
        return NoEdge;
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
