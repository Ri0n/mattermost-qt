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
#include <QTimer>

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

    if (ui && ui->listWidget && !property("threadReadStateConnected").toBool()) {
        setProperty("threadReadStateConnected", true);

        // ThreadPostSource is connected to onNewPost before this UI observer, so
        // by the time this callback runs the semantic tail already contains the
        // incoming reply. The queued read check then waits for its concrete row.
        connect(&channel, &BackendChannel::onNewPost, this,
                [this](BackendPost& post) {
            if (post.root_id == root_id && !post.isOwnPost()) {
                requestThreadReadAcknowledgement();
            }
        });

        // Replies received while the user was reading older history remain
        // unread. Reaching the newest edge is the reading gesture that may
        // acknowledge them.
        connect(ui->listWidget, &LongListWidget::userViewportChanged, this,
                [this](bool atEnd) {
            if (atEnd && threadReadPending) {
                requestThreadReadAcknowledgement();
            }
        });

        // A live reply may reserve its logical tail before the PostWidget is
        // materialized. Retry only while a semantic read is pending; ordinary
        // reflow/materialization never creates read state on its own.
        connect(ui->listWidget, &LongListWidget::materializedRangeChanged, this,
                [this](int, int) {
            if (threadReadPending) {
                QTimer::singleShot(0, this,
                                   &ChatArea::tryThreadReadAcknowledgement);
            }
        });
    }

    if (threadReadPending) {
        QTimer::singleShot(0, this, &ChatArea::tryThreadReadAcknowledgement);
    }
}

} // namespace Mattermost
