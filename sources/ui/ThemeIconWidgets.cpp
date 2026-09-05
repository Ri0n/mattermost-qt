/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#include "ThemeIconWidgets.h"

#include <utility>

#include <QApplication>
#include <QColor>
#include <QIcon>
#include <QPainter>
#include <QPalette>

#include "IconUtils.h"
#include "ThemeDebug.h"

namespace Mattermost {
namespace {

constexpr qreal RestingIconOpacity = 0.8;

QString tintKey(const QColor& color)
{
    return color.name(QColor::HexArgb);
}

} // namespace

ThemeIconButton::ThemeIconButton(QWidget* parent)
    : QPushButton(parent)
{
}

QString ThemeIconButton::symbolicResource() const
{
    if (objectName() == QStringLiteral("addEmojiButton")) {
        return QStringLiteral(":/icons/emoji");
    }
    if (objectName() == QStringLiteral("attachButton")) {
        return QStringLiteral(":/icons/paperclip");
    }
    return {};
}

void ThemeIconButton::paintEvent(QPaintEvent* event)
{
    const QString resource = symbolicResource();
    if (!resource.isEmpty()) {
        QColor desired = qApp
            ? qApp->palette().color(QPalette::ButtonText)
            : palette().color(QPalette::ButtonText);
        if (!underMouse()) {
            desired.setAlphaF(desired.alphaF() * RestingIconOpacity);
        }

        const QString desiredTint = tintKey(desired);
        const bool externalIconReplacement = renderedIconCacheKey != 0
            && icon().cacheKey() != renderedIconCacheKey;
        if (renderedTint != desiredTint || externalIconReplacement) {
            ThemeDebug::logWidgetState(
                objectName() == QStringLiteral("addEmojiButton")
                    ? "CHAT_AREA_ICON_PAINT_REBUILD_EMOJI"
                    : "CHAT_AREA_ICON_PAINT_REBUILD_ATTACH",
                this, QEvent::Paint);

            const QIcon rebuilt = IconUtils::tintedSymbolicIcon(resource, desired);
            QPushButton::setIcon(rebuilt);
            renderedTint = desiredTint;
            renderedIconCacheKey = rebuilt.cacheKey();
        }
    }

    QPushButton::paintEvent(event);
}

ThemeSymbolicIconLabel::ThemeSymbolicIconLabel(QString resourcePathValue,
                                               QWidget* parent)
    : QLabel(parent)
    , resourcePath(std::move(resourcePathValue))
{
}

void ThemeSymbolicIconLabel::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    const QColor desired = qApp
        ? qApp->palette().color(QPalette::WindowText)
        : palette().color(QPalette::WindowText);
    const QString desiredTint = tintKey(desired);
    if (renderedTint != desiredTint || renderedPixmap.size() != size()) {
        ThemeDebug::logWidgetState("THREAD_ICON_PAINT_REBUILD", this, QEvent::Paint);
        const QIcon icon = IconUtils::tintedSymbolicIcon(resourcePath, desired);
        renderedPixmap = icon.pixmap(size());
        renderedTint = desiredTint;
    }

    QPainter painter(this);
    if (!renderedPixmap.isNull()) {
        const QPoint topLeft((width() - renderedPixmap.width()) / 2,
                             (height() - renderedPixmap.height()) / 2);
        painter.drawPixmap(topLeft, renderedPixmap);
    }
}

} // namespace Mattermost
