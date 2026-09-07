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

#include "TransientScrollBarStyle.h"

namespace Mattermost {

TransientScrollBarStyle::TransientScrollBarStyle(QStyle* baseStyle)
    : QProxyStyle(baseStyle)
{
}

int TransientScrollBarStyle::styleHint(StyleHint hint,
                                       const QStyleOption* option,
                                       const QWidget* widget,
                                       QStyleHintReturn* returnData) const
{
    if (hint == SH_ScrollBar_Transient) {
        return 1;
    }
    return QProxyStyle::styleHint(hint, option, widget, returnData);
}

} // namespace Mattermost
