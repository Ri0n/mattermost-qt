/**
 * @file ChannelIcons.h
 * @brief Lightweight fallback icons for channel rows without user avatars.
 *
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <QApplication>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>

namespace Mattermost {
namespace ChannelIcons {

inline QIcon channel()
{
    QIcon themed = QIcon::fromTheme(QStringLiteral("irc-channel-active"));
    if (!themed.isNull()) {
        return themed;
    }

    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QApplication::palette().color(QPalette::Text));
    QFont font = QApplication::font();
    font.setBold(true);
    font.setPixelSize(18);
    painter.setFont(font);
    painter.drawText(pixmap.rect(), Qt::AlignCenter, QStringLiteral("#"));
    return QIcon(pixmap);
}

inline QIcon groupConversation()
{
    QIcon themed = QIcon::fromTheme(QStringLiteral("system-users"));
    if (!themed.isNull()) {
        return themed;
    }

    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QApplication::palette().color(QPalette::Text));

    painter.drawEllipse(QRectF(4.0, 4.0, 7.0, 7.0));
    painter.drawEllipse(QRectF(13.0, 5.0, 6.0, 6.0));

    QPainterPath body;
    body.addRoundedRect(QRectF(2.5, 12.0, 10.5, 7.0), 3.5, 3.5);
    body.addRoundedRect(QRectF(11.5, 13.0, 10.0, 6.0), 3.0, 3.0);
    painter.drawPath(body);
    return QIcon(pixmap);
}

} // namespace ChannelIcons
} // namespace Mattermost
