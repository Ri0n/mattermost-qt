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
