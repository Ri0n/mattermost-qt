/**
 * @file OutgoingPostPanel.cpp
 * @brief
 * @author Lyubomir Filipov
 * @date Mar 04, 2022
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

#include "OutgoingPostPanel.h"

#include <algorithm>

#include <QEvent>
#include <QFont>
#include <QGraphicsOpacityEffect>
#include <QHideEvent>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QShowEvent>
#include <QSizePolicy>
#include <QTimer>

#include "MessageTextEditWidget.h"
#include "OutgoingPostCreator.h"
#include "ui/IconUtils.h"
#include "ui/ThemeDebug.h"
#include "ui_OutgoingPostPanel.h"

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

OutgoingPostPanel::OutgoingPostPanel(QWidget *parent)
:QWidget(parent)
,ui(new Ui::OutgoingPostPanel)
{
    ui->setupUi(this);

    auto configureActionButton = [](QPushButton& button) {
        button.setFlat(true);
        button.setFixedSize(ActionButtonExtent, ActionButtonExtent);
        button.setCursor(Qt::PointingHandCursor);
        // Breeze still paints a hover frame for flat QPushButton. Keep styling
        // local to the button so the rest of the widget tree remains fully
        // palette-driven while the action itself stays visually borderless.
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

    refreshActionIcons();

    loadingIndicator = new LoadingIndicator(this);
    ui->horizontalLayout->insertWidget(0, loadingIndicator, 0, Qt::AlignVCenter);

    loadingDelayTimer = new QTimer(this);
    loadingDelayTimer->setSingleShot(true);
    loadingDelayTimer->setInterval(LoadingIndicatorDelayMs);
    connect(loadingDelayTimer, &QTimer::timeout, this, [this] {
        if (pendingMessageLoads > 0 && loadingIndicator) {
            loadingIndicator->show();
        }
    });
}

OutgoingPostPanel::~OutgoingPostPanel()
{
    delete ui;
}

QPushButton& OutgoingPostPanel::attachButton ()
{
	return *ui->attachButton;
}

QPushButton& OutgoingPostPanel::addEmojiButton ()
{
	return *ui->addEmojiButton;
}

QPushButton& OutgoingPostPanel::sendButton ()
{
	return *ui->sendButton;
}

QLabel& OutgoingPostPanel::label ()
{
	return *ui->label;
}

void OutgoingPostPanel::beginMessageLoading()
{
    ++pendingMessageLoads;
    if (pendingMessageLoads == 1 && loadingIndicator && !loadingIndicator->isVisible()) {
        loadingDelayTimer->start();
    }
}

void OutgoingPostPanel::endMessageLoading()
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

void OutgoingPostPanel::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (!event) {
        return;
    }

    if (event->type() == QEvent::PaletteChange
        || event->type() == QEvent::ApplicationPaletteChange) {
        ThemeDebug::logWidgetState("OUTGOING_CHANGE_HANDLER", this, event->type());
        if (loadingIndicator) {
            loadingIndicator->update();
        }
    }
}

bool OutgoingPostPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (!event) {
        return QWidget::eventFilter(watched, event);
    }

    const bool iconVisualChanged = event->type() == QEvent::PaletteChange
        || event->type() == QEvent::EnabledChange
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
                              "OUTGOING_ICON_REFRESH_EMOJI",
                              hovered);
        } else if (watched == ui->attachButton) {
            refreshActionIcon(*ui->attachButton,
                              QStringLiteral(":/icons/paperclip"),
                              "OUTGOING_ICON_REFRESH_ATTACH",
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

void OutgoingPostPanel::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    adoptComposerWidget();
    focusComposer();
}

void OutgoingPostPanel::adoptComposerWidget()
{
    if (composerWidget || !ui || !parentWidget()) {
        return;
    }

    auto* composer = parentWidget()->findChild<OutgoingPostCreator*>(
        QString(), Qt::FindDirectChildrenOnly);
    if (!composer) {
        return;
    }

    composerWidget = composer;
    composerWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    // Status/loading stay at the far left. The actual compose controls are
    // ordered like Telegram: attach | editor | emoji | send.
    ui->horizontalLayout->removeWidget(ui->attachButton);
    ui->horizontalLayout->removeWidget(ui->addEmojiButton);
    ui->horizontalLayout->removeWidget(ui->sendButton);
    ui->horizontalLayout->addWidget(ui->attachButton, 0, Qt::AlignVCenter);
    ui->horizontalLayout->addWidget(composerWidget, 1, Qt::AlignVCenter);
    ui->horizontalLayout->addWidget(ui->addEmojiButton, 0, Qt::AlignVCenter);
    ui->horizontalLayout->addWidget(ui->sendButton, 0, Qt::AlignVCenter);

    // 40% of line height is 20% less than the previous half-line padding.
    const int verticalPadding = std::max(
        2, composerWidget->fontMetrics().lineSpacing() * 2 / 5);
    ui->horizontalLayout->setContentsMargins(0, verticalPadding, 0, verticalPadding);
}

void OutgoingPostPanel::focusComposer()
{
    if (!composerWidget) {
        return;
    }

    if (auto* editor = composerWidget->findChild<MessageTextEditWidget*>()) {
        editor->setFocus(Qt::OtherFocusReason);
    }
}

void OutgoingPostPanel::refreshActionIcons()
{
    if (!ui) {
        return;
    }

    refreshActionIcon(*ui->addEmojiButton,
                      QStringLiteral(":/icons/emoji"),
                      "OUTGOING_ICON_REFRESH_EMOJI",
                      ui->addEmojiButton->underMouse());
    refreshActionIcon(*ui->attachButton,
                      QStringLiteral(":/icons/paperclip"),
                      "OUTGOING_ICON_REFRESH_ATTACH",
                      ui->attachButton->underMouse());
}

void OutgoingPostPanel::refreshActionIcon(QPushButton& button,
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

} /* namespace Mattermost */
