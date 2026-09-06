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
inline constexpr char ComposerMessageLoadingProperty[] = "_mmqt_composer_message_loading";

/**
 * Borderless composer action button. The normal QPushButton style is not
 * painted at all, so desktop styles cannot reintroduce a hover frame and a
 * QStyleSheetStyle wrapper is unnecessary. Both symbolic icons and the textual
 * send glyph are drawn from the same current application palette.
 *
 * The attach action also owns the composer's transient busy presentation. A
 * send/upload operation uses ComposerBusyTextProperty while chat-history
 * loading uses ComposerMessageLoadingProperty. Either state replaces the
 * paperclip in-place with the same centered compact spinner, keeping composer
 * geometry stable. Independent properties allow overlapping operations to end
 * without clearing each other's busy presentation.
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
