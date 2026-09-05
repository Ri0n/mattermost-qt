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

#include <QEvent>
#include <QHideEvent>
#include <QLabel>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QShowEvent>
#include <QTimer>

#include "ui/IconUtils.h"
#include "ui_OutgoingPostPanel.h"

namespace Mattermost {
namespace {

constexpr int LoadingIndicatorDelayMs = 150;
constexpr int LoadingIndicatorExtent = 18;
constexpr int LoadingAnimationIntervalMs = 70;

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

    ui->addEmojiButton->setText(QString());
    ui->addEmojiButton->setIconSize(QSize(18, 18));

    ui->attachButton->setText(QString());
    ui->attachButton->setIconSize(QSize(18, 18));
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
        refreshActionIcons();
        if (loadingIndicator) {
            loadingIndicator->update();
        }
    }
}

void OutgoingPostPanel::refreshActionIcons()
{
    if (!ui) {
        return;
    }

    const QColor emojiColor = ui->addEmojiButton->palette().color(QPalette::ButtonText);
    const QColor attachColor = ui->attachButton->palette().color(QPalette::ButtonText);
    ui->addEmojiButton->setIcon(
        IconUtils::tintedSymbolicIcon(QStringLiteral(":/icons/emoji"), emojiColor));
    ui->attachButton->setIcon(
        IconUtils::tintedSymbolicIcon(QStringLiteral(":/icons/paperclip"), attachColor));
}

} /* namespace Mattermost */
