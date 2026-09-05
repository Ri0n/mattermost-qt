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
