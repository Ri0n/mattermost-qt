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

#include "ChannelHeaderTextLabel.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <QDesktopServices>
#include <QEvent>
#include <QSizePolicy>
#include <QTextBrowser>
#include <QTextDocument>

#include "ChatArea.h"
#include "backend/emoji/EmojiRegistryNotifier.h"
#include "navigation/AppNavigationService.h"
#include "post/MessageFormatter.h"
#include "ui/EmojiPresentation.h"
#include "ui/PresenceAvatarLabel.h"

namespace Mattermost {

ChannelHeaderTextLabel::ChannelHeaderTextLabel(QWidget* parent)
    : QLabel(parent)
{
    setTextFormat(Qt::RichText);
    setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::TextSelectableByMouse);
    setOpenExternalLinks(false);
    setWordWrap(false);
    setMinimumWidth(0);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    installEventFilter(this);

    connect(this, &QLabel::linkActivated, this, [this](const QString& href) {
        openLink(QUrl(href));
    });
    connect(&EmojiRegistryNotifier::instance(),
            &EmojiRegistryNotifier::customEmojiAdded,
            this,
            [this](const QString& name) {
        const QString token = QLatin1Char(':') + name + QLatin1Char(':');
        if (!sourceText.contains(token)) {
            return;
        }
        // Topics are often rendered before the asynchronous custom-emoji
        // download finishes. Reformat the original source once the referenced
        // emoji enters EmojiInfo rather than leaving the literal :name: text.
        setText(sourceText);
    });

    hideTimer.setSingleShot(true);
    hideTimer.setInterval(120);
    connect(&hideTimer, &QTimer::timeout, this, &ChannelHeaderTextLabel::hidePopover);

    updateCollapsedHeight();
}

void ChannelHeaderTextLabel::setText(const QString& text)
{
    // A direct-message presence is presentation state, not channel header text.
    // Route it to the avatar badge so we use the same AvatarUtils visual as the
    // timeline and never depend on rich-text foreground palette propagation.
    if (PresenceAvatarLabel::isPresenceStatus(text)) {
        sourceText.clear();
        formattedText.clear();
        QLabel::clear();
        hidePopover();
        if (popover) {
            popover->clear();
        }
        if (QWidget* host = parentWidget()) {
            if (auto* avatar = host->findChild<PresenceAvatarLabel*>(
                    QStringLiteral("userAvatar"))) {
                avatar->setStatus(text);
            }
        }
        hide();
        return;
    }

    sourceText = text;

    // QTextDocument::toHtml() returns a complete HTML document even for an
    // empty Markdown source. Keep QLabel::text() genuinely empty here because
    // ChatArea uses text().isEmpty() to decide whether a DM presence value still
    // needs to be installed.
    if (text.isEmpty()) {
        formattedText.clear();
        QLabel::setText(QString());
        updateCollapsedHeight();
        hidePopover();
        if (popover) {
            popover->clear();
        }
        hide();
        return;
    }

    show();
    formattedText = EmojiPresentation::normalizeHtml(
        MessageFormatter::formatMessageText(text),
        font(),
        EmojiPresentation::Mode::Inline);
    QLabel::setText(formattedText);
    updateCollapsedHeight();

    if (popover) {
        popover->setHtml(formattedText);
        if (popover->isVisible()) {
            if (isOverflowing()) {
                positionPopover();
            } else {
                hidePopover();
            }
        }
    }
}

void ChannelHeaderTextLabel::setLinkHandler(LinkHandler handler)
{
    linkHandler = std::move(handler);
}

QSize ChannelHeaderTextLabel::sizeHint() const
{
    return QSize(0, std::max(1, fontMetrics().lineSpacing() + 6));
}

QSize ChannelHeaderTextLabel::minimumSizeHint() const
{
    return QSize(0, std::max(1, fontMetrics().lineSpacing() + 6));
}

void ChannelHeaderTextLabel::updateCollapsedHeight()
{
    const int height = std::max(1, fontMetrics().lineSpacing() + 6);
    setMinimumHeight(height);
    setMaximumHeight(height);
}

bool ChannelHeaderTextLabel::isOverflowing() const
{
    if (sourceText.isEmpty() || width() <= 0) {
        return false;
    }

    if (sourceText.contains(QLatin1Char('\n'))) {
        return true;
    }

    QTextDocument document;
    document.setDefaultFont(font());
    document.setDocumentMargin(0);
    document.setHtml(formattedText);

    if (document.blockCount() > 1) {
        return true;
    }

    return std::ceil(document.idealWidth()) > std::max(1, width() - 4);
}

void ChannelHeaderTextLabel::ensurePopover()
{
    QWidget* host = nullptr;
    for (QWidget* candidate = this; candidate; candidate = candidate->parentWidget()) {
        if (qobject_cast<ChatArea*>(candidate)) {
            host = candidate;
            break;
        }
    }
    if (!host) {
        host = window();
    }
    if (!host) {
        return;
    }

    if (popover && popover->parentWidget() == host) {
        return;
    }

    if (popover) {
        popover->deleteLater();
    }

    auto* browser = new QTextBrowser(host);
    browser->setObjectName(QStringLiteral("channelHeaderTextPopover"));
    browser->setReadOnly(true);
    browser->setOpenExternalLinks(false);
    browser->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::TextSelectableByMouse);
    browser->setFrameShape(QFrame::Box);
    browser->setFrameShadow(QFrame::Plain);
    browser->setLineWidth(1);
    browser->setAutoFillBackground(true);
    browser->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    browser->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    browser->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    browser->document()->setDocumentMargin(8);
    browser->setHtml(formattedText);
    connect(browser, &QTextBrowser::anchorClicked, this,
            [this](const QUrl& url) { openLink(url); });
    browser->hide();
    browser->installEventFilter(this);
    browser->viewport()->installEventFilter(this);
    popover = browser;
}

void ChannelHeaderTextLabel::positionPopover()
{
    if (!popover) {
        return;
    }

    QWidget* host = popover->parentWidget();
    if (!host) {
        return;
    }

    // This is an expansion of the ChatArea header rather than an independent
    // card. Use the exact panel background and the entire chat-side width.
    QPalette popoverPalette = popover->palette();
    const QColor panelBackground = host->palette().color(QPalette::Window);
    popoverPalette.setColor(QPalette::Base, panelBackground);
    popoverPalette.setColor(QPalette::Window, panelBackground);
    popover->setPalette(popoverPalette);

    const QPoint labelPos = mapTo(host, QPoint(0, 0));
    const int popupWidth = std::max(1, host->width());

    // Measure independently from the browser's pre-show viewport so the first
    // hover and every later hover use identical wrapping and height.
    QTextDocument measure;
    measure.setDefaultFont(popover->font());
    measure.setDocumentMargin(8);
    measure.setHtml(formattedText);
    measure.setTextWidth(std::max(1, popupWidth - 16));
    const int documentHeight = static_cast<int>(std::ceil(measure.size().height())) + 2;

    const int top = std::max(0, labelPos.y());
    const int availableHeight = std::max(1, host->height() - top);
    const int popupHeight = std::min(std::max(height(), documentHeight), availableHeight);

    popover->document()->setTextWidth(std::max(1, popupWidth - 16));
    popover->setGeometry(0, top, popupWidth, popupHeight);
    popover->raise();
}

void ChannelHeaderTextLabel::showPopover()
{
    hideTimer.stop();
    if (!isOverflowing()) {
        return;
    }

    ensurePopover();
    if (!popover) {
        return;
    }

    popover->setHtml(formattedText);
    positionPopover();
    popover->show();
    popover->raise();
}

void ChannelHeaderTextLabel::hidePopoverSoon()
{
    hideTimer.start();
}

void ChannelHeaderTextLabel::hidePopover()
{
    hideTimer.stop();
    if (popover) {
        popover->hide();
    }
}

void ChannelHeaderTextLabel::openLink(const QUrl& url)
{
    if (!url.isValid()) {
        return;
    }
    if (linkHandler) {
        linkHandler(url);
        return;
    }

    // The header is specific to ChatArea, so use its semantic navigation as the
    // default route. AppNavigationService keeps external URLs in the browser and
    // handles local channel/DM/permalink URLs inside the application.
    for (QWidget* host = parentWidget(); host; host = host->parentWidget()) {
        if (auto* area = qobject_cast<ChatArea*>(host)) {
            AppNavigationService::instance(area->getBackend()).openUrl(url);
            return;
        }
    }
    QDesktopServices::openUrl(url);
}

bool ChannelHeaderTextLabel::eventFilter(QObject* watched, QEvent* event)
{
    const bool isLabel = watched == this;
    const bool isPopover = popover
        && (watched == popover.data() || watched == popover->viewport());

    if (isLabel || isPopover) {
        switch (event->type()) {
        case QEvent::Enter:
            hideTimer.stop();
            if (isLabel) {
                showPopover();
            }
            break;
        case QEvent::Leave:
            hidePopoverSoon();
            break;
        case QEvent::Resize:
            if (isLabel && popover && popover->isVisible()) {
                if (isOverflowing()) {
                    positionPopover();
                } else {
                    hidePopover();
                }
            }
            break;
        case QEvent::Hide:
            if (isLabel) {
                hidePopover();
            }
            break;
        case QEvent::FontChange:
            if (isLabel) {
                updateCollapsedHeight();
                if (!sourceText.isEmpty()) {
                    setText(sourceText);
                }
            }
            break;
        case QEvent::PaletteChange:
        case QEvent::ApplicationPaletteChange:
            if (popover && popover->isVisible()) {
                positionPopover();
            }
            break;
        default:
            break;
        }
    }

    return QLabel::eventFilter(watched, event);
}

} // namespace Mattermost
