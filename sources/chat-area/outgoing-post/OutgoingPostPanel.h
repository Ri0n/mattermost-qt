/**
 * @file OutgoingPostPanel.h
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

#pragma once

#include <QWidget>

class QEvent;
class QLabel;
class QPushButton;
class QShowEvent;
class QTimer;

namespace Ui {
class OutgoingPostPanel;
}

namespace Mattermost {

class OutgoingPostCreator;

class OutgoingPostPanel: public QWidget {
    Q_OBJECT
public:
    explicit OutgoingPostPanel(QWidget *parent = nullptr);
    ~OutgoingPostPanel();
public:
    OutgoingPostCreator& composer();
    QPushButton& attachButton();
    QPushButton& addEmojiButton();
    QPushButton& sendButton();
    QLabel& label();

public slots:
    /** Track one LongList range request. The indicator itself is delayed by 150 ms. */
    void beginMessageLoading();
    /** Complete one LongList range request. Overlapping requests are reference-counted. */
    void endMessageLoading();

protected:
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void focusComposer();
    void refreshActionIcons();
    void refreshActionIcon(QPushButton& button,
                           const QString& resourcePath,
                           const char* debugMarker,
                           bool hovered);

    Ui::OutgoingPostPanel *ui;
    QWidget* loadingIndicator = nullptr;
    QTimer* loadingDelayTimer = nullptr;
    int pendingMessageLoads = 0;
};

} /* namespace Mattermost */
