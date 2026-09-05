/**
 * Copyright 2026 Sergei Ilinykh
 *
 * Adapted from AnyKeep's IconUtils by the same author.
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

#include "IconUtils.h"

#include <QColor>
#include <QGuiApplication>
#include <QIcon>
#include <QPainter>
#include <QPalette>
#include <QPixmap>

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#include <QStyleHints>
#endif

namespace Mattermost {

bool IconUtils::isDarkColorScheme()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    const auto scheme = QGuiApplication::styleHints()->colorScheme();
    if (scheme == Qt::ColorScheme::Dark) {
        return true;
    }
    if (scheme == Qt::ColorScheme::Light) {
        return false;
    }
#endif

    return QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
}

QIcon IconUtils::tintedIcon(const QIcon& source, const QColor& color)
{
    if (source.isNull() || !color.isValid()) {
        return {};
    }

    QIcon icon;
    for (int size : {16, 20, 22, 24, 32, 48}) {
        QPixmap pixmap = source.pixmap(size, size);
        if (pixmap.isNull()) {
            continue;
        }

        QPainter painter(&pixmap);
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(pixmap.rect(), color);
        icon.addPixmap(pixmap);
    }
    return icon;
}

QIcon IconUtils::tintedSymbolicIcon(const QString& path, const QColor& color)
{
    return tintedIcon(QIcon(path), color);
}

QIcon IconUtils::symbolicIcon(const QString& path)
{
    const QColor color = QGuiApplication::palette().color(QPalette::WindowText);
    QIcon icon = tintedSymbolicIcon(path, color);
    return icon.isNull() ? QIcon(path) : icon;
}

} // namespace Mattermost
