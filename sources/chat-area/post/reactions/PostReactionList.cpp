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

#include "PostReactionList.h"

#include "PostReaction.h"
#include "ui_PostReactionList.h"

namespace Mattermost {

PostReactionList::PostReactionList(Backend& backend, QWidget* parent)
    : QWidget(parent)
    , backend_(backend)
    , ui_(new Ui::PostReactionList)
{
    ui_->setupUi(this);
}

PostReactionList::~PostReactionList()
{
    delete ui_;
}

void PostReactionList::addReaction(const QString& emojiName,
                                   const QString& emojiValue,
                                   const BackendPostReaction& reactionData)
{
    auto* reaction = new PostReaction(
        backend_, emojiName, emojiValue, reactionData, this);
    connect(reaction, &PostReaction::clicked,
            this, &PostReactionList::reactionClicked);
    ui_->horizontalLayout_2->addWidget(reaction, 0, Qt::AlignLeft);
}

} /* namespace Mattermost */
