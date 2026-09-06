/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#pragma once

#include <QPixmap>
#include <QPushButton>
#include <QString>

class QEvent;
class QPaintEvent;
class QWidget;

namespace Mattermost {

/**
 * Borderless composer action button. The normal QPushButton style is not
 * painted at all, so desktop styles cannot reintroduce a hover frame and a
 * QStyleSheetStyle wrapper is unnecessary. Both symbolic icons and the textual
 * send glyph are drawn from the same current application palette.
 */
class ThemeIconButton final : public QPushButton
{
public:
    explicit ThemeIconButton(QWidget* parent = nullptr);

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    QString symbolicResource() const;

    QString renderedTint;
    QSize renderedSize;
    QPixmap renderedPixmap;
};

} // namespace Mattermost
