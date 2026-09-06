/**
 * Copyright 2026 Sergei Ilinykh
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
 * along with Mattermost-QT. If not, see https://www.gnu.org/licenses/.
 */

#include "ChatArea.h"

#include <QShowEvent>

#include "AbstractPostSource.h"
#include "ThreadWindowTitle.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"

namespace Mattermost {

void ChatArea::updateThreadWindowTitle()
{
    if (!isThread || root_id.isEmpty()) {
        return;
    }

    BackendPost* rootPost = channel.postIdToPost.value(root_id, nullptr);
    if (rootPost) {
        setWindowTitle(threadWindowTitle(channel, *rootPost));
        return;
    }

    QString channelName = channel.display_name.trimmed();
    if (channelName.isEmpty()) {
        channelName = QStringLiteral("Mattermost");
    }
    setWindowTitle(channelName + QStringLiteral(" — Thread"));
}

void ChatArea::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    focusComposer();

    if (!isThread) {
        return;
    }

    updateThreadWindowTitle();

    // A permalink can create the thread window before the root post itself has
    // reached the local channel cache. The ThreadPostSource already owns that
    // asynchronous load, so simply refresh the title when its identity/content
    // becomes available instead of issuing another network request here.
    if (postSource && !property("threadTitleSourceConnected").toBool()) {
        setProperty("threadTitleSourceConnected", true);
        connect(postSource, &AbstractPostSource::rangeAvailable, this,
                [this](int, int) { updateThreadWindowTitle(); });
        connect(postSource, &AbstractPostSource::itemsChanged, this,
                [this](int, int) { updateThreadWindowTitle(); });
        connect(postSource, &AbstractPostSource::itemCountChanged, this,
                [this](int) { updateThreadWindowTitle(); });
    }
}

} // namespace Mattermost
