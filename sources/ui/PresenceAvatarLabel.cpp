/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#include "PresenceAvatarLabel.h"

#include <algorithm>

#include <QApplication>
#include <QEvent>
#include <QPaintEvent>
#include <QPalette>
#include <QResizeEvent>

#include "AvatarUtils.h"

namespace Mattermost {

namespace {

constexpr int DefaultAvatarSize = 48;
constexpr int DefaultBadgeSize = 12;

QColor applicationWindowColor()
{
    return qApp ? qApp->palette().color(QPalette::Window) : QColor();
}

} // namespace

PresenceAvatarLabel::PresenceAvatarLabel(QWidget* parent)
    : QLabel(parent)
{
    setFrameShape(QFrame::NoFrame);
    setAlignment(Qt::AlignCenter);
}

void PresenceAvatarLabel::setPixmap(const QPixmap& pixmap)
{
    sourcePixmap = pixmap;
    refreshPixmap();
}

void PresenceAvatarLabel::setStatus(const QString& status)
{
    if (presenceStatus == status) {
        return;
    }
    presenceStatus = status;
    refreshPixmap();
}

bool PresenceAvatarLabel::isPresenceStatus(const QString& text)
{
    return text == QStringLiteral("online")
        || text == QStringLiteral("away")
        || text == QStringLiteral("dnd")
        || text == QStringLiteral("offline");
}

void PresenceAvatarLabel::changeEvent(QEvent* event)
{
    QLabel::changeEvent(event);
    if (event && (event->type() == QEvent::PaletteChange
                  || event->type() == QEvent::ApplicationPaletteChange
                  || event->type() == QEvent::StyleChange)) {
        refreshPixmap();
    }
}

void PresenceAvatarLabel::paintEvent(QPaintEvent* event)
{
    // The badge contains a one-pixel background ring. Validate that cached
    // pixmap at the actual paint boundary so a desktop palette transition can
    // never leave that ring in the previous theme's background colour.
    const QColor currentBackground = applicationWindowColor();
    if (renderedBackground != currentBackground) {
        refreshPixmap();
    }
    QLabel::paintEvent(event);
}

void PresenceAvatarLabel::resizeEvent(QResizeEvent* event)
{
    QLabel::resizeEvent(event);
    refreshPixmap();
}

void PresenceAvatarLabel::refreshPixmap()
{
    if (sourcePixmap.isNull()) {
        renderedBackground = applicationWindowColor();
        QLabel::clear();
        return;
    }

    int avatarSize = std::min(width(), height());
    if (avatarSize <= 0) {
        avatarSize = DefaultAvatarSize;
    }
    const int badgeSize = std::max(4,
        qRound(DefaultBadgeSize * avatarSize / static_cast<qreal>(DefaultAvatarSize)));

    renderedBackground = applicationWindowColor();
    QLabel::setPixmap(AvatarUtils::withStatus(sourcePixmap,
                                               avatarSize,
                                               presenceStatus,
                                               badgeSize,
                                               renderedBackground));
}

PresenceStatusLabel::PresenceStatusLabel(QWidget* parent)
    : QLabel(parent)
{
}

void PresenceStatusLabel::setText(const QString& text)
{
    if (PresenceAvatarLabel::isPresenceStatus(text)) {
        if (QWidget* host = parentWidget()) {
            if (auto* avatar = host->findChild<PresenceAvatarLabel*>(
                    QStringLiteral("usericon_label"))) {
                avatar->setStatus(text);
                QLabel::clear();
                hide();
                return;
            }
        }
    }

    QLabel::setText(text);
    setVisible(!text.isEmpty());
}

} // namespace Mattermost
