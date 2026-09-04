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

#include <QtGlobal>

namespace Mattermost {

class TimelineSeekState
{
public:
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
    }

    bool setTarget(int index)
    {
        if (index == targetIndex) {
            return false;
        }
        ++generation;
        targetIndex = index;
        ready = false;
        return true;
    }

    void markReady()
    {
        ready = targetIndex >= 0;
    }

    void complete(const Ticket& ticket)
    {
        if (isCurrent(ticket)) {
            ready = false;
        }
    }

    bool isReady() const { return ready && targetIndex >= 0; }
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

private:
    quint64 generation = 0;
    int targetIndex = -1;
    bool ready = false;
};

} // namespace Mattermost
