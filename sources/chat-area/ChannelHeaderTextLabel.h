/**
 * Copyright 2026 Sergei Ilinykh
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

#pragma once

#include <QLabel>
#include <QPointer>
#include <QTimer>

class QTextBrowser;

namespace Mattermost {

/**
 * Compact channel-header text with Mattermost-like Markdown hover expansion.
 *
 * The collapsed label remains one line high. Overflowing/multiline text is
 * shown in an overlay QTextBrowser while hovered, so expanding the header does
 * not change the chat layout or move the currently visible posts.
 */
class ChannelHeaderTextLabel final: public QLabel
{
    Q_OBJECT
public:
    explicit ChannelHeaderTextLabel(QWidget* parent = nullptr);

    // QLabel::setText() is not virtual, but ui_ChatArea stores this concrete
    // type, so ChatArea's existing calls resolve to this formatting wrapper.
    void setText(const QString& text);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    bool isOverflowing() const;
    void ensurePopover();
    void showPopover();
    void positionPopover();
    void hidePopoverSoon();
    void hidePopover();
    void updateCollapsedHeight();

    QString sourceText;
    QString formattedText;
    QPointer<QTextBrowser> popover;
    QTimer hideTimer;
};

} // namespace Mattermost
