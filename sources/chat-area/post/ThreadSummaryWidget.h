/**
 * Copyright 2026 Sergei Ilinykh
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
 * along with Mattermost-QT. If not, see https://www.gnu.org/licenses/.
 */

#pragma once

#include <QWidget>

class QEvent;
class QHBoxLayout;
class QLabel;

namespace Mattermost {

class Backend;
class BackendChannel;
class BackendPost;
class BackendUser;

class ThreadSummaryWidget final : public QWidget
{
    Q_OBJECT
public:
    ThreadSummaryWidget(Backend& backend,
                        BackendChannel& channel,
                        BackendPost& rootPost,
                        QWidget* parent = nullptr);

signals:
    void clicked();

public slots:
    void refresh();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void rebuildParticipantAvatars();
    void watchUser(const BackendUser* user);

    Backend& backend;
    BackendChannel& channel;
    BackendPost& rootPost;
    QHBoxLayout* layout = nullptr;
    QWidget* chip = nullptr;
    QLabel* chipIcon = nullptr;
    QLabel* chipCount = nullptr;
};

} // namespace Mattermost
