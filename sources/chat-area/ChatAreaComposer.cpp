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
#include <QGraphicsOpacityEffect>
#include <QHideEvent>
#include <QPainter>
#include <QPalette>
#include <QPointer>
#include <QPushButton>
#include <QShowEvent>
#include <QTimer>

#include "ChatLogWidget.h"
#include "ui/IconUtils.h"
#include "ui/ThemeDebug.h"
#include "ui_ChatArea.h"

namespace Mattermost {
namespace {

constexpr int LoadingIndicatorDelayMs = 150;
constexpr int LoadingIndicatorExtent = 18;
constexpr int LoadingAnimationIntervalMs = 70;
constexpr int ActionButtonExtent = 30;
constexpr int ActionIconExtent = 24;
constexpr qreal RestingIconOpacity = 0.8;

class LoadingIndicator final : public QWidget
{
public:
    explicit LoadingIndicator(QWidget* parent = nullptr)
        : QWidget(parent)
        , animationTimer(this)
    {
        setFixedSize(LoadingIndicatorExtent, LoadingIndicatorExtent);
        setToolTip(tr("Loading messages"));
        setAccessibleName(tr("Loading messages"));
        setAttribute(Qt::WA_TransparentForMouseEvents, true);

        animationTimer.setInterval(LoadingAnimationIntervalMs);
        connect(&animationTimer, &QTimer::timeout, this, [this] {
            phase = (phase + 1) % 12;
            update();
        });
        hide();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        QColor color = palette().color(QPalette::WindowText);
        color.setAlpha(190);
        QPen pen(color, 2.0, Qt::SolidLine, Qt::RoundCap);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);

        const QRectF ring(2.5, 2.5, width() - 5.0, height() - 5.0);
        painter.drawArc(ring, (-90 + phase * 30) * 16, 105 * 16);
    }

    void showEvent(QShowEvent* event) override
    {
        QWidget::showEvent(event);
        animationTimer.start();
    }

    void hideEvent(QHideEvent* event) override
    {
        animationTimer.stop();
        QWidget::hideEvent(event);
    }

private:
    QTimer animationTimer;
    int phase = 0;
};

} // namespace

void ChatArea::setupComposerUi()
{
    auto configureActionButton = [](QPushButton& button) {
        button.setFlat(true);
        button.setFixedSize(ActionButtonExtent, ActionButtonExtent);
        button.setCursor(Qt::PointingHandCursor);
        // Breeze still paints a hover frame for flat QPushButton. Keep styling
        // local to the button so palette propagation through ChatArea stays native.
        button.setStyleSheet(QStringLiteral(
            "QPushButton { border: none; background: transparent; padding: 0px; margin: 0px; }"));
    };

    ui->addEmojiButton->setText(QString());
    ui->addEmojiButton->setIconSize(QSize(ActionIconExtent, ActionIconExtent));
    configureActionButton(*ui->addEmojiButton);
    ui->addEmojiButton->installEventFilter(this);

    ui->attachButton->setText(QString());
    ui->attachButton->setIconSize(QSize(ActionIconExtent, ActionIconExtent));
    configureActionButton(*ui->attachButton);
    ui->attachButton->installEventFilter(this);

    configureActionButton(*ui->sendButton);
    QFont sendFont = ui->sendButton->font();
    if (sendFont.pointSizeF() > 0.0) {
        sendFont.setPointSizeF(sendFont.pointSizeF() + 2.0);
    } else if (sendFont.pixelSize() > 0) {
        sendFont.setPixelSize(sendFont.pixelSize() + 3);
    }
    ui->sendButton->setFont(sendFont);
    ui->sendButton->installEventFilter(this);

    auto* sendOpacity = new QGraphicsOpacityEffect(ui->sendButton);
    sendOpacity->setOpacity(RestingIconOpacity);
    ui->sendButton->setGraphicsEffect(sendOpacity);

    // The editor is the only vertically growing child. The other controls are
    // bottom-aligned in ChatArea.ui, so new lines grow upward from the action row.
    const int verticalPadding = std::max(
        2, ui->outgoingPostCreator->fontMetrics().lineSpacing() * 2 / 5);
    ui->composerLayout->setContentsMargins(0, verticalPadding, 0, verticalPadding);

    refreshActionIcons();

    loadingIndicator = new LoadingIndicator(this);
    ui->composerLayout->insertWidget(0, loadingIndicator, 0, Qt::AlignBottom);

    loadingDelayTimer = new QTimer(this);
    loadingDelayTimer->setSingleShot(true);
    loadingDelayTimer->setInterval(LoadingIndicatorDelayMs);
    connect(loadingDelayTimer, &QTimer::timeout, this, [this] {
        if (pendingMessageLoads > 0 && loadingIndicator) {
            loadingIndicator->show();
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
    if (pendingMessageLoads == 1 && loadingIndicator && !loadingIndicator->isVisible()) {
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

    loadingDelayTimer->stop();
    if (loadingIndicator) {
        loadingIndicator->hide();
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
        ThemeDebug::logWidgetState("CHAT_AREA_CHANGE_HANDLER", this, event->type());

        // Palette events are delivered top-down. At this point a child button
        // may still expose its previous palette, which is why rebuilding a
        // tinted pixmap synchronously leaves the old light/dark colour cached.
        // Rebuild after the event queue has propagated the new palette through
        // all composer children.
        QPointer<ChatArea> guard(this);
        QTimer::singleShot(0, this, [guard] {
            if (!guard || !guard->ui) {
                return;
            }
            guard->refreshActionIcons();
            if (guard->loadingIndicator) {
                guard->loadingIndicator->update();
            }
        });
    }
}

bool ChatArea::eventFilter(QObject* watched, QEvent* event)
{
    if (!event) {
        return QWidget::eventFilter(watched, event);
    }

    const bool paletteChanged = event->type() == QEvent::PaletteChange
        || event->type() == QEvent::ApplicationPaletteChange
        || event->type() == QEvent::StyleChange;
    if (paletteChanged) {
        // Event filters run before the watched widget processes PaletteChange.
        // Use a queued refresh rather than tinting from the stale button palette.
        QPointer<ChatArea> guard(this);
        QTimer::singleShot(0, this, [guard] {
            if (guard && guard->ui) {
                guard->refreshActionIcons();
            }
        });
        return QWidget::eventFilter(watched, event);
    }

    const bool iconVisualChanged = event->type() == QEvent::EnabledChange
        || event->type() == QEvent::Enter
        || event->type() == QEvent::Leave;

    if (iconVisualChanged) {
        bool hovered = false;
        if (event->type() == QEvent::Enter) {
            hovered = true;
        } else if (event->type() != QEvent::Leave) {
            if (auto* button = qobject_cast<QPushButton*>(watched)) {
                hovered = button->underMouse();
            }
        }

        if (watched == ui->addEmojiButton) {
            refreshActionIcon(*ui->addEmojiButton,
                              QStringLiteral(":/icons/emoji"),
                              "CHAT_AREA_ICON_REFRESH_EMOJI",
                              hovered);
        } else if (watched == ui->attachButton) {
            refreshActionIcon(*ui->attachButton,
                              QStringLiteral(":/icons/paperclip"),
                              "CHAT_AREA_ICON_REFRESH_ATTACH",
                              hovered);
        } else if (watched == ui->sendButton) {
            if (auto* effect = qobject_cast<QGraphicsOpacityEffect*>(
                    ui->sendButton->graphicsEffect())) {
                effect->setOpacity(hovered && ui->sendButton->isEnabled()
                                       ? 1.0 : RestingIconOpacity);
            }
        }
    }

    return QWidget::eventFilter(watched, event);
}

void ChatArea::refreshActionIcons()
{
    refreshActionIcon(*ui->addEmojiButton,
                      QStringLiteral(":/icons/emoji"),
                      "CHAT_AREA_ICON_REFRESH_EMOJI",
                      ui->addEmojiButton->underMouse());
    refreshActionIcon(*ui->attachButton,
                      QStringLiteral(":/icons/paperclip"),
                      "CHAT_AREA_ICON_REFRESH_ATTACH",
                      ui->attachButton->underMouse());
}

void ChatArea::refreshActionIcon(QPushButton& button,
                                 const QString& resourcePath,
                                 const char* debugMarker,
                                 bool hovered)
{
    ThemeDebug::logWidgetState(debugMarker, &button, QEvent::None);

    QColor color = button.palette().color(QPalette::ButtonText);
    if (!hovered) {
        color.setAlphaF(color.alphaF() * RestingIconOpacity);
    }
    button.setIcon(IconUtils::tintedSymbolicIcon(resourcePath, color));
}

} // namespace Mattermost
