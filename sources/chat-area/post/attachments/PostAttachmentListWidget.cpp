/**
 * @file PostAttachmentListWidget.cpp
 * @brief
 * @author Lyubomir Filipov
 * @date Jan 21, 2022
 *
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Mattermost-QT is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Mattermost-QT. if not, see https://www.gnu.org/licenses/.
 */

#include <algorithm>

#include "PostAttachmentListWidget.h"

namespace Mattermost {

QSize PostAttachmentListWidget::sizeHint () const
{
    int width = 0;
    int height = 0;

    for (int i = 0; i < count(); ++i) {
        const QListWidgetItem* listItem = item(i);
        const QSize itemSize = listItem->sizeHint();
        width = std::max(width, itemSize.width());
        height += itemSize.height();
    }

    if (count() > 1) {
        height += spacing() * (count() - 1);
    }

    const int frame = 2 * frameWidth();
    return QSize(width + frame, height + frame);
}

} /* namespace Mattermost */
