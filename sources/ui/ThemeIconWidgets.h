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
inline constexpr char ThemeIconResourceProperty[] = "_mmqt_theme_icon_resource";
inline constexpr char ThemeIconBusyProperty[] = "_mmqt_theme_icon_busy";

/**
 * Borderless palette-aware action button.
 *
 * A symbolic resource supplied through ThemeIconResourceProperty is tinted from
 * the current application palette every time the theme changes. Setting
 * ThemeIconBusyProperty replaces the icon in-place with the shared compact
 * spinner. Composer buttons retain their historical object-name behavior, so
 * callers can opt into the generic properties without changing existing UI
 * files.
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
    void invalidateRenderedIcon();

    QString _renderedTint;
    QString _renderedResource;
    QSize _renderedSize;
    QPixmap _renderedPixmap;
    QTimer _busyAnimationTimer;
    int _busyPhase = 0;
};

} // namespace Mattermost
