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

#include "PostListWidget.h"

#include <QFrame>

namespace Mattermost {

PostListWidget::PostListWidget(QWidget* parent)
    : LongListWidget(parent)
{
    setFrameShape(QFrame::NoFrame);
    setLineWidth(0);
    setMidLineWidth(0);
    setViewportMargins(0, 0, 0, 0);

    setMaterializationLimit(200);
    setRequestBlockSize(10);
    setPrefetchScreens(1);
    setSeekDebounceMs(100);
}

} // namespace Mattermost
