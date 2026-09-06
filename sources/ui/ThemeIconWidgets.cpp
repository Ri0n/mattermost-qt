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
constexpr int BusyAnimationIntervalMs = 70;
constexpr int BusyAnimationSteps = 12;
constexpr int BusyIndicatorExtent = 18;

QString tintKey(const QColor& color)
{
    return color.name(QColor::HexArgb);
}

} // namespace

ThemeIconButton::ThemeIconButton(QWidget* parent)
    : QPushButton(parent)
{
    setCursor(Qt::PointingHandCursor);

    busyAnimationTimer.setInterval(BusyAnimationIntervalMs);
    connect(&busyAnimationTimer, &QTimer::timeout, this, [this] {
        busyPhase = (busyPhase + 1) % BusyAnimationSteps;
        update();
    });
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

bool ThemeIconButton::isBusy() const
{
    return objectName() == QStringLiteral("attachButton")
        && !property(ComposerBusyTextProperty).toString().isEmpty();
}

void ThemeIconButton::syncBusyAnimation()
{
    if (isBusy()) {
        if (!busyAnimationTimer.isActive()) {
            busyAnimationTimer.start();
        }
    } else {
        busyAnimationTimer.stop();
        busyPhase = 0;
    }
    update();
}

bool ThemeIconButton::event(QEvent* event)
{
    const QEvent::Type type = event ? event->type() : QEvent::None;
    const bool result = QPushButton::event(event);

    if (type == QEvent::DynamicPropertyChange) {
        syncBusyAnimation();
    } else if (type == QEvent::Enter
               || type == QEvent::Leave
               || type == QEvent::EnabledChange
               || type == QEvent::PaletteChange
               || type == QEvent::ApplicationPaletteChange
               || type == QEvent::StyleChange) {
        update();
    }
    return result;
}

void ThemeIconButton::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    const QPalette currentPalette = qApp ? qApp->palette() : palette();

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    if (isBusy()) {
        QColor busyColor = currentPalette.color(QPalette::WindowText);
        busyColor.setAlpha(190);
        painter.setPen(QPen(busyColor, 2.0, Qt::SolidLine, Qt::RoundCap));
        painter.setBrush(Qt::NoBrush);

        const qreal indicatorExtent = BusyIndicatorExtent;
        const QRectF ring((width() - indicatorExtent) / 2.0 + 2.5,
                          (height() - indicatorExtent) / 2.0 + 2.5,
                          indicatorExtent - 5.0,
                          indicatorExtent - 5.0);
        painter.drawArc(ring, (-90 + busyPhase * 30) * 16, 105 * 16);
        return;
    }

    const QPalette::ColorGroup group = isEnabled()
        ? QPalette::Active : QPalette::Disabled;
    QColor color = currentPalette.color(group, QPalette::ButtonText);
    if (!underMouse()) {
        color.setAlphaF(color.alphaF() * RestingOpacity);
    }

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
