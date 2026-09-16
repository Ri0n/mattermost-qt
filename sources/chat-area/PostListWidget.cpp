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

#include "post/PostWidget.h"

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
    setPrefetchScreens(0);
    setSeekDebounceMs(100);

    connect(this, &LongListWidget::hoveredItemChanged, this,
            [this](int previousIndex, int currentIndex) {
        if (auto* previous = qobject_cast<PostWidget*>(itemWidget(previousIndex))) {
            previous->setHovered(false);
        }
        if (auto* current = qobject_cast<PostWidget*>(itemWidget(currentIndex))) {
            current->setHovered(true);
        }
    });
}

QString PostListWidget::itemIdentity(const QWidget* widget) const
{
    const auto* postWidget = qobject_cast<const PostWidget*>(widget);
    return postWidget ? postWidget->post.id : QString();
}

} // namespace Mattermost
