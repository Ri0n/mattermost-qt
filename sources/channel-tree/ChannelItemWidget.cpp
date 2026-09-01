/**
 * Copyright 2021, 2022 Lyubomir Filipov
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
 * along with Mattermost-QT. if not, see https://www.gnu.org/licenses/.
 */

#include "ChannelItemWidget.h"
#include "ui_ChannelItemWidget.h"

#include <QStyle>

ChannelItemWidget::ChannelItemWidget(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::ChannelItemWidget)
{
    ui->setupUi (this);
    ui->icon->setVisible (false);
    ui->mutedIcon->setPixmap(style()->standardIcon(QStyle::SP_MediaVolumeMuted).pixmap(16, 16));
    ui->mutedIcon->setVisible(false);
    defaultLabelPalette = ui->label->palette();
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

void ChannelItemWidget::setMuted(bool muted)
{
    ui->mutedIcon->setVisible(muted);

    if (!muted) {
        ui->label->setPalette(defaultLabelPalette);
        return;
    }

    QPalette mutedPalette = defaultLabelPalette;
    mutedPalette.setColor(QPalette::WindowText,
                          defaultLabelPalette.color(QPalette::Disabled, QPalette::WindowText));
    ui->label->setPalette(mutedPalette);
}

void ChannelItemWidget::setMentioned(bool mentioned)
{
    QFont font = ui->label->font();
    font.setBold(mentioned);
    ui->label->setFont(font);
}
