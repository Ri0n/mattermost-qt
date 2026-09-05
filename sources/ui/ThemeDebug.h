/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <QEvent>
#include <QObject>

class QWidget;

namespace Mattermost {
namespace ThemeDebug {

void logWidgetState(const char* marker,
                    const QWidget* widget,
                    QEvent::Type eventType = QEvent::None);

} // namespace ThemeDebug

/**
 * Temporary diagnostics for live palette propagation.
 *
 * The probe is deliberately read-only: it never changes palettes, styles,
 * widgets or repaint state. Enable it with:
 *
 *   QT_LOGGING_RULES='mattermost.theme.trace.debug=true'
 */
class ThemeDebugProbe final : public QObject
{
public:
    explicit ThemeDebugProbe(QObject* parent = nullptr);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
};

} // namespace Mattermost
