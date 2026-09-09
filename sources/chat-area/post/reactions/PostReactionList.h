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

#pragma once

#include <QVector>
#include <QWidget>

namespace Ui {
class PostReactionList;
}

namespace Mattermost {

class Backend;

using BackendPostReaction = QVector<QString>;

class PostReactionList: public QWidget
{
    Q_OBJECT
public:
    explicit PostReactionList(Backend& backend, QWidget* parent = nullptr);
    ~PostReactionList();

    void addReaction(const QString& emojiName,
                     const QString& emojiValue,
                     const BackendPostReaction& reactionData);

signals:
    void reactionClicked(const QString& emojiName);

private:
    Backend& backend_;
    Ui::PostReactionList* ui_;
};

} /* namespace Mattermost */
