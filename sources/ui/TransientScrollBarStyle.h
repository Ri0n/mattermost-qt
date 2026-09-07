/*
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

#include <QProxyStyle>

namespace Mattermost {

/**
 * Preserve the current platform style while asking Qt scroll areas to use
 * transient/overlay scroll bars. Standard Qt styles then own hover, fade and
 * painting behavior instead of the application hard-coding a scrollbar theme.
 */
class TransientScrollBarStyle final : public QProxyStyle
{
public:
    explicit TransientScrollBarStyle(QStyle* baseStyle = nullptr);

    int styleHint(StyleHint hint,
                  const QStyleOption* option = nullptr,
                  const QWidget* widget = nullptr,
                  QStyleHintReturn* returnData = nullptr) const override;
};

} // namespace Mattermost
