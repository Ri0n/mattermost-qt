/**
 * @file PostEvent.cpp
 * @brief
 * @author Lyubomir Filipov
 * @date Dec 31, 2021
 *
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

#include <QJsonDocument>
#include "PostEvent.h"

namespace Mattermost {

PostEvent::PostEvent (const QJsonObject& data, const QJsonObject& broadcast)
:teamId (data.value("team_id").toString())
,channelId (broadcast.value ("channel_id").toString())
//the post object is a JSON string inside the data object, with escaped json elements
,postObject (QJsonDocument::fromJson (data.value ("post").toString().toUtf8()).object())
,set_online (data.value("set_online").toBool())
{
    // Mattermost adds `mentions` to a posted websocket event only for a
    // recipient who was actually mentioned. Keep that recipient-specific bit
    // on the local post so notification/UI code does not need to parse text.
    postObject.insert(QStringLiteral("_mmqt_current_user_mentioned"),
                      !data.value(QStringLiteral("mentions")).toString().isEmpty());

    // `sender_name` lives next to the serialized post in websocket events, not
    // inside the Post object itself. Preserve it as transient client metadata
    // so an arriving post can still identify its sender before that user's
    // profile has been loaded into Storage.
    postObject.insert(QStringLiteral("_mmqt_sender_name"),
                      data.value(QStringLiteral("sender_name")).toString());
}

PostEvent::~PostEvent () = default;

} /* namespace Mattermost */
