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
#include "ChatLogWidget.h"
#include "ThreadWindowTitle.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "ui_ChatArea.h"

namespace Mattermost {

void ChatArea::updateThreadWindowTitle()
{
    if (!isThread || root_id.isEmpty()) {
        return;
    }

    ui->statusLabel->setPresenceRoutingEnabled(false);

    BackendPost* rootPost = channel.postIdToPost.value(root_id, nullptr);
    if (rootPost) {
        ui->statusLabel->setText(rootPost->isDeleted
            ? tr("(Message deleted)") : rootPost->message);
        setWindowTitle(threadWindowTitle(channel, *rootPost));
        return;
    }

    // A thread window can be constructed before its root reaches the local
    // cache. Keep the compact header empty until ThreadPostSource publishes the
    // body; the existing source signals below will refresh it immediately.
    ui->statusLabel->setText(QString());

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
        connect(postSource, &AbstractPostSource::layoutChanged, this,
                [this](int, int) { updateThreadWindowTitle(); });
        connect(postSource, &AbstractPostSource::itemCountChanged, this,
                [this](int) { updateThreadWindowTitle(); });
    }

    // Becoming visible is enough reason to re-check the read cursor. The check
    // itself remains purely viewport based; showing/raising a thread does not
    // mark anything read unless a post lower edge is actually visible.
    if (ui && ui->listWidget) {
        ui->listWidget->refreshReadState();
    }
}

} // namespace Mattermost
