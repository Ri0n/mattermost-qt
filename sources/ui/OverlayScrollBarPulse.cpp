/*
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "OverlayScrollBarManager.h"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QScrollBar>

namespace Mattermost {

bool OverlayScrollBarManager::pulse(QAbstractScrollArea& area)
{
    auto* application = qApp;
    if (!application) {
        return false;
    }

    OverlayScrollBarManager* manager = nullptr;
    for (QObject* child : application->children()) {
        manager = dynamic_cast<OverlayScrollBarManager*>(child);
        if (manager) {
            break;
        }
    }
    if (!manager) {
        return false;
    }

    if (!manager->states.contains(&area)) {
        manager->registerArea(&area);
    }
    State* state = manager->states.value(&area, nullptr);
    if (!state) {
        return false;
    }

    const auto isScrollable = [](const QScrollBar* bar) {
        return bar && bar->maximum() > bar->minimum();
    };
    if (!isScrollable(area.verticalScrollBar()) && !isScrollable(area.horizontalScrollBar())) {
        return false;
    }

    manager->reveal(*state);
    manager->scheduleFade(*state);
    return true;
}

} // namespace Mattermost
