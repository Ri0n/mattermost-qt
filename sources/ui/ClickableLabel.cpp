/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#include "ClickableLabel.h"

#include <QMouseEvent>

namespace Mattermost {

ClickableLabel::ClickableLabel(QWidget* parent)
    : QLabel(parent)
{
    setCursor(Qt::PointingHandCursor);
}

void ClickableLabel::mouseReleaseEvent(QMouseEvent* event)
{
    QLabel::mouseReleaseEvent(event);
    if (event && event->button() == Qt::LeftButton && rect().contains(event->pos())) {
        emit clicked();
    }
}

} // namespace Mattermost
