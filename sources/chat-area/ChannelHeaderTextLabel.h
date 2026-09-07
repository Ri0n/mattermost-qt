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
 * along with Mattermost-QT. if not, see https://www.gnu.org/licenses/.
 */

#pragma once

#include <functional>

#include <QLabel>
#include <QPointer>
#include <QTimer>
#include <QUrl>

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
    using LinkHandler = std::function<void(const QUrl&)>;

    explicit ChannelHeaderTextLabel(QWidget* parent = nullptr);

    // QLabel::setText() is not virtual, but ui_ChatArea stores this concrete
    // type, so ChatArea's existing calls resolve to this formatting wrapper.
    void setText(const QString& text);
    void setLinkHandler(LinkHandler handler);

    // Rich-text QLabel uses its unwrapped document width as a minimum hint.
    // A topic must never dictate a thread-window or chat-pane width.
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

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
    void openLink(const QUrl& url);

    QString sourceText;
    QString formattedText;
    QPointer<QTextBrowser> popover;
    QTimer hideTimer;
    LinkHandler linkHandler;
};

} // namespace Mattermost
