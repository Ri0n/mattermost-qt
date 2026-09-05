/**
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

#include "ChannelItemWidget.h"
#include "ui_ChannelItemWidget.h"

#include <QEvent>
#include <QPalette>
#include <QStyle>

ChannelItemWidget::ChannelItemWidget(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::ChannelItemWidget)
{
    ui->setupUi (this);
    ui->icon->setVisible (false);
    ui->mutedIcon->setVisible(false);
    refreshTheme();
}

ChannelItemWidget::~ChannelItemWidget()
{
    delete ui;
}

const QPixmap ChannelItemWidget::getPixmap () const
{
#if QT_VERSION <= QT_VERSION_CHECK(5,15,0)
	return ui->icon->pixmap () ? *ui->icon->pixmap () : QPixmap();
#else
	return ui->icon->pixmap (Qt::ReturnByValue);
#endif
}

void ChannelItemWidget::setIcon (const QIcon& icon)
{
	ui->icon->setPixmap (icon.pixmap(24, 24));
	ui->icon->setVisible (true);
}

void ChannelItemWidget::setLabel (const QString& label)
{
	ui->label->setText (label);
}

void ChannelItemWidget::setMuted(bool isMuted)
{
    muted = isMuted;
    refreshTheme();
}

void ChannelItemWidget::setMentioned(bool mentioned)
{
    QFont font = ui->label->font();
    font.setBold(mentioned);
    ui->label->setFont(font);
}

void ChannelItemWidget::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event && (event->type() == QEvent::PaletteChange
                  || event->type() == QEvent::ApplicationPaletteChange
                  || event->type() == QEvent::StyleChange)) {
        refreshTheme();
    }
}

void ChannelItemWidget::refreshTheme()
{
    ui->mutedIcon->setPixmap(style()->standardIcon(QStyle::SP_MediaVolumeMuted).pixmap(16, 16));
    ui->mutedIcon->setVisible(muted);

    // Do not cache a fully resolved palette: that freezes the foreground from
    // whichever desktop theme was active when this row was constructed.
    ui->label->setPalette(QPalette());
    if (!muted) {
        return;
    }

    QPalette mutedOverride;
    mutedOverride.setColor(QPalette::WindowText,
                           palette().color(QPalette::Disabled, QPalette::WindowText));
    ui->label->setPalette(mutedOverride);
}
