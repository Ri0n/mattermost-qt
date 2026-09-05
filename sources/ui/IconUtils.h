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

#pragma once

#include <QString>

class QColor;
class QIcon;

namespace Mattermost {

/** Helpers for palette-aware symbolic icons. */
class IconUtils
{
public:
    static bool isDarkColorScheme();
    static QIcon tintedIcon(const QIcon& icon, const QColor& color);
    static QIcon tintedSymbolicIcon(const QString& path, const QColor& color);
    static QIcon symbolicIcon(const QString& path);
};

} // namespace Mattermost
