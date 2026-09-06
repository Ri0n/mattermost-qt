/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#include "ThemeIconWidgets.h"

#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QIcon>
#include <QPainter>
#include <QPalette>

#include "IconUtils.h"

namespace Mattermost {
namespace {

constexpr qreal RestingOpacity = 0.8;

QString tintKey(const QColor& color)
{
    return color.name(QColor::HexArgb);
}

} // namespace

ThemeIconButton::ThemeIconButton(QWidget* parent)
    : QPushButton(parent)
{
    setCursor(Qt::PointingHandCursor);
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

bool ThemeIconButton::event(QEvent* event)
{
    const bool result = QPushButton::event(event);
    if (event && (event->type() == QEvent::Enter
                  || event->type() == QEvent::Leave
                  || event->type() == QEvent::EnabledChange
                  || event->type() == QEvent::PaletteChange
                  || event->type() == QEvent::ApplicationPaletteChange
                  || event->type() == QEvent::StyleChange)) {
        update();
    }
    return result;
}

void ThemeIconButton::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    const QPalette currentPalette = qApp ? qApp->palette() : palette();
    const QPalette::ColorGroup group = isEnabled()
        ? QPalette::Active : QPalette::Disabled;
    QColor color = currentPalette.color(group, QPalette::ButtonText);
    if (!underMouse()) {
        color.setAlphaF(color.alphaF() * RestingOpacity);
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    const QString resource = symbolicResource();
    if (!resource.isEmpty()) {
        const QSize targetSize = iconSize().isValid() ? iconSize() : QSize(24, 24);
        const QString desiredTint = tintKey(color);
        if (renderedTint != desiredTint || renderedSize != targetSize) {
            renderedPixmap = IconUtils::tintedSymbolicIcon(resource, color).pixmap(targetSize);
            renderedTint = desiredTint;
            renderedSize = targetSize;
        }

        if (!renderedPixmap.isNull()) {
            const QPoint topLeft((width() - renderedPixmap.width()) / 2,
                                 (height() - renderedPixmap.height()) / 2);
            painter.drawPixmap(topLeft, renderedPixmap);
        }
        return;
    }

    painter.setPen(color);
    painter.setFont(font());
    painter.drawText(rect(), Qt::AlignCenter, text());
}

} // namespace Mattermost
