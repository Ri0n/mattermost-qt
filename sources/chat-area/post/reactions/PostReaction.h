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

class QMouseEvent;

namespace Ui {
class PostReaction;
}

namespace Mattermost {

class Backend;

using BackendPostReaction = QVector<QString>;

class PostReaction: public QWidget
{
    Q_OBJECT
public:
    explicit PostReaction(Backend& backend,
                          const QString& emojiName,
                          const QString& emojiValue,
                          const BackendPostReaction& reactionData,
                          QWidget* parent = nullptr);
    ~PostReaction();

signals:
    void clicked(const QString& emojiName);

protected:
    void mousePressEvent(QMouseEvent* event) override;

private:
    void updateToolTip();

    Backend& backend_;
    QString emojiName_;
    QString emojiValue_;
    BackendPostReaction reactionData_;
    bool profileLookupFinished_ = false;
    Ui::PostReaction* ui_;
};

} /* namespace Mattermost */
