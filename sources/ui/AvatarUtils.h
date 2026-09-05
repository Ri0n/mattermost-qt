/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Mattermost-QT is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Mattermost-QT. If not, see https://www.gnu.org/licenses/.
 */

#pragma once

#include <algorithm>

#include <QColor>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QString>

namespace Mattermost::AvatarUtils {

inline QColor statusColor(const QString& status)
{
    if (status == QStringLiteral("online")) {
        return QColor(QStringLiteral("#3DB887"));
    }
    if (status == QStringLiteral("away")) {
        return QColor(QStringLiteral("#FFBC1F"));
    }
    if (status == QStringLiteral("dnd")) {
        return QColor(QStringLiteral("#D24B4E"));
    }
    return QColor(QStringLiteral("#8E8E8E"));
}

inline void drawStatusBadge(QPainter* painter,
                            const QRectF& rect,
                            const QString& status,
                            const QColor& backgroundColor)
{
    if (!painter || status.isEmpty() || rect.isEmpty()) {
        return;
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    // Clear a small ring around the badge with the row/avatar background. This
    // is the same visual treatment used by the sidebar contact rows.
    painter->setPen(Qt::NoPen);
    painter->setBrush(backgroundColor);
    painter->drawEllipse(rect.adjusted(-1.0, -1.0, 1.0, 1.0));

    const qreal scale = rect.width() / 8.0;
    if (status == QStringLiteral("online")) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(statusColor(status));
        painter->drawEllipse(rect);

        QPen pen(Qt::white);
        pen.setWidthF(1.15 * scale);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter->setPen(pen);
        painter->setBrush(Qt::NoBrush);

        QPainterPath check;
        check.moveTo(rect.left() + 1.8 * scale, rect.top() + 4.1 * scale);
        check.lineTo(rect.left() + 3.3 * scale, rect.top() + 5.5 * scale);
        check.lineTo(rect.left() + 6.3 * scale, rect.top() + 2.4 * scale);
        painter->drawPath(check);
    } else if (status == QStringLiteral("away")) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(statusColor(status));
        painter->drawEllipse(rect);

        QPen pen(Qt::white);
        pen.setWidthF(1.0 * scale);
        pen.setCapStyle(Qt::RoundCap);
        painter->setPen(pen);
        const QPointF center = rect.center();
        painter->drawLine(center, QPointF(center.x(), rect.top() + 2.0 * scale));
        painter->drawLine(center,
                          QPointF(rect.right() - 1.7 * scale,
                                  center.y() + 1.0 * scale));
    } else if (status == QStringLiteral("dnd")) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(statusColor(status));
        painter->drawEllipse(rect);

        QPen pen(Qt::white);
        pen.setWidthF(1.25 * scale);
        pen.setCapStyle(Qt::RoundCap);
        painter->setPen(pen);
        painter->drawLine(QPointF(rect.left() + 2.0 * scale, rect.center().y()),
                          QPointF(rect.right() - 2.0 * scale, rect.center().y()));
    } else {
        QPen pen(statusColor(QStringLiteral("online")));
        pen.setWidthF(1.25 * scale);
        painter->setPen(pen);
        painter->setBrush(backgroundColor);
        painter->drawEllipse(rect.adjusted(0.65 * scale, 0.65 * scale,
                                           -0.65 * scale, -0.65 * scale));
    }

    painter->restore();
}

inline QPixmap circular(const QPixmap& source, int size)
{
    if (source.isNull() || size <= 0) {
        return {};
    }

    QPixmap result(size, size);
    result.fill(Qt::transparent);

    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    QPainterPath clip;
    clip.addEllipse(QRectF(0, 0, size, size));
    painter.setClipPath(clip);

    const QPixmap scaled = source.scaled(size, size,
                                         Qt::KeepAspectRatioByExpanding,
                                         Qt::SmoothTransformation);
    const int x = (size - scaled.width()) / 2;
    const int y = (size - scaled.height()) / 2;
    painter.drawPixmap(x, y, scaled);
    return result;
}

inline QPixmap withStatus(const QPixmap& source,
                          int size,
                          const QString& status,
                          int badgeSize,
                          const QColor& backgroundColor)
{
    QPixmap result = circular(source, size);
    if (result.isNull() || status.isEmpty()) {
        return result;
    }

    badgeSize = std::max(4, std::min(badgeSize, size));
    QPainter painter(&result);
    const QRectF badge(size - badgeSize - 1,
                       size - badgeSize - 1,
                       badgeSize,
                       badgeSize);
    drawStatusBadge(&painter, badge, status, backgroundColor);
    return result;
}

} // namespace Mattermost::AvatarUtils
