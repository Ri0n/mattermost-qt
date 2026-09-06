/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
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

#include <algorithm>

#include <QEvent>
#include <QFont>
#include <QPushButton>
#include <QTimer>

#include "ChatLogWidget.h"
#include "QuotedReplyController.h"
#include "ui/ThemeIconWidgets.h"
#include "ui_ChatArea.h"

namespace Mattermost {
namespace {

constexpr int LoadingIndicatorDelayMs = 150;
constexpr int ActionButtonExtent = 30;
constexpr int ActionIconExtent = 24;

} // namespace

void ChatArea::setupComposerUi()
{
    auto configureActionButton = [](QPushButton& button) {
        button.setFixedSize(ActionButtonExtent, ActionButtonExtent);
        button.setCursor(Qt::PointingHandCursor);
    };

    ui->addEmojiButton->setText(QString());
    ui->addEmojiButton->setIconSize(QSize(ActionIconExtent, ActionIconExtent));
    configureActionButton(*ui->addEmojiButton);

    ui->attachButton->setText(QString());
    ui->attachButton->setIconSize(QSize(ActionIconExtent, ActionIconExtent));
    configureActionButton(*ui->attachButton);

    configureActionButton(*ui->sendButton);
    QFont sendFont = ui->sendButton->font();
    if (sendFont.pointSizeF() > 0.0) {
        sendFont.setPointSizeF(sendFont.pointSizeF() + 2.0);
    } else if (sendFont.pixelSize() > 0) {
        sendFont.setPixelSize(sendFont.pixelSize() + 3);
    }
    ui->sendButton->setFont(sendFont);

    // No textual status belongs to the left of the input: it changes the
    // editor's horizontal geometry. Transient send and history-loading state
    // share the fixed attach-action slot, and persistent edit/reply state lives
    // above the editor as a compact context preview.
    ui->composerStatusLabel->hide();

    // The action buttons are owner-drawn by ThemeIconButton. Do not attach a
    // stylesheet merely to suppress Breeze's hover frame: QStyleSheetStyle was
    // also the source of stale inherited palette roles during theme changes.

    // The editor is the only vertically growing child. The other controls are
    // bottom-aligned in ChatArea.ui, so new lines grow upward from the action row.
    const int verticalPadding = std::max(
        2, ui->outgoingPostCreator->fontMetrics().lineSpacing() * 2 / 5);
    ui->composerLayout->setContentsMargins(0, verticalPadding, 0, verticalPadding);

    // Install the context controller eagerly so it also observes edit mode,
    // which can be entered without first using quoted replies.
    QuotedReplyController::instance(*this);

    loadingDelayTimer = new QTimer(this);
    loadingDelayTimer->setSingleShot(true);
    loadingDelayTimer->setInterval(LoadingIndicatorDelayMs);
    connect(loadingDelayTimer, &QTimer::timeout, this, [this] {
        if (pendingMessageLoads > 0 && ui && ui->attachButton) {
            ui->attachButton->setProperty(ComposerMessageLoadingProperty, true);
        }
    });

    connect(ui->listWidget, &LongListWidget::rangeRequested, this,
            [this](int, int, LongListWidget::RequestReason, quint64) {
        beginMessageLoading();
    });
    connect(ui->listWidget, &LongListWidget::rangeRequestFinished, this,
            [this](int, int) {
        endMessageLoading();
    });
}

void ChatArea::focusComposer()
{
    if (ui && ui->outgoingPostCreator) {
        ui->outgoingPostCreator->setFocus(Qt::OtherFocusReason);
    }
}

void ChatArea::beginMessageLoading()
{
    ++pendingMessageLoads;
    if (pendingMessageLoads == 1 && loadingDelayTimer
        && ui && ui->attachButton
        && !ui->attachButton->property(ComposerMessageLoadingProperty).toBool()) {
        loadingDelayTimer->start();
    }
}

void ChatArea::endMessageLoading()
{
    if (pendingMessageLoads <= 0) {
        return;
    }

    --pendingMessageLoads;
    if (pendingMessageLoads != 0) {
        return;
    }

    if (loadingDelayTimer) {
        loadingDelayTimer->stop();
    }
    if (ui && ui->attachButton) {
        ui->attachButton->setProperty(ComposerMessageLoadingProperty, false);
    }
}

void ChatArea::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (!event) {
        return;
    }

    if (event->type() == QEvent::PaletteChange
        || event->type() == QEvent::ApplicationPaletteChange
        || event->type() == QEvent::StyleChange) {
        if (ui) {
            ui->addEmojiButton->update();
            ui->attachButton->update();
            ui->sendButton->update();
        }
    }
}

} // namespace Mattermost
