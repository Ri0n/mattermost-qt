/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#pragma once

#include <QPixmap>
#include <QPushButton>
#include <QString>
#include <QTimer>

class QEvent;
class QPaintEvent;
class QWidget;

namespace Mattermost {

inline constexpr char ComposerBusyTextProperty[] = "_mmqt_composer_busy_text";

/**
 * Borderless composer action button. The normal QPushButton style is not
 * painted at all, so desktop styles cannot reintroduce a hover frame and a
 * QStyleSheetStyle wrapper is unnecessary. Both symbolic icons and the textual
 * send glyph are drawn from the same current application palette.
 *
 * The attach action also owns the composer's transient busy presentation. When
 * ComposerBusyTextProperty is non-empty, the paperclip is replaced in-place by
 * the same compact spinner used for message loading, keeping composer geometry
 * stable while a post or attachment is in flight.
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
    bool isBusy() const;
    void syncBusyAnimation();

    QString renderedTint;
    QSize renderedSize;
    QPixmap renderedPixmap;
    QTimer busyAnimationTimer;
    int busyPhase = 0;
};

} // namespace Mattermost
