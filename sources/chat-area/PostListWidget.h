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

#pragma once

#include "widgets/LongListWidget.h"

namespace Mattermost {

/**
 * Shared presentation policy for virtualized lists whose rows contain posts.
 *
 * LongListWidget deliberately owns only generic virtualization and scrolling.
 * PostListWidget adds the visual/list tuning common to chat timelines and
 * Saved/Search/Pinned post collections so those views cannot drift apart.
 */
class PostListWidget : public LongListWidget
{
public:
    explicit PostListWidget(QWidget* parent = nullptr);
};

} // namespace Mattermost
