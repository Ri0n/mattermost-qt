/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#pragma once

#include <QLabel>
#include <QPushButton>
#include <QString>

class QPaintEvent;
class QWidget;

namespace Mattermost {

/**
 * Composer action button whose symbolic icon is validated at the actual paint
 * boundary against QApplication's current palette. This deliberately avoids
 * depending on PaletteChange delivery order through styled child widgets.
 */
class ThemeIconButton final : public QPushButton
{
public:
    explicit ThemeIconButton(QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString symbolicResource() const;
    QString renderedTint;
    qint64 renderedIconCacheKey = 0;
};

/**
 * Palette-aware symbolic label. The label paints its own icon rather than
 * exposing a cached QLabel pixmap, so a stale pixmap cannot survive a system
 * theme transition.
 */
class ThemeSymbolicIconLabel final : public QLabel
{
public:
    explicit ThemeSymbolicIconLabel(QString resourcePath,
                                    QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString resourcePath;
    QString renderedTint;
    QPixmap renderedPixmap;
};

} // namespace Mattermost
