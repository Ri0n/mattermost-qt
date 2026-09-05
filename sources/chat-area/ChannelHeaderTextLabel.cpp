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

#include "ChannelHeaderTextLabel.h"

#include <algorithm>
#include <cmath>

#include <QEvent>
#include <QTextBrowser>
#include <QTextDocument>

#include "post/MessageFormatter.h"

namespace Mattermost {

ChannelHeaderTextLabel::ChannelHeaderTextLabel(QWidget* parent)
    : QLabel(parent)
{
    setTextFormat(Qt::RichText);
    setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::TextSelectableByMouse);
    setOpenExternalLinks(true);
    setWordWrap(false);
    installEventFilter(this);

    hideTimer.setSingleShot(true);
    hideTimer.setInterval(120);
    connect(&hideTimer, &QTimer::timeout, this, &ChannelHeaderTextLabel::hidePopover);

    updateCollapsedHeight();
}

void ChannelHeaderTextLabel::setText(const QString& text)
{
    sourceText = text;

    // QTextDocument::toHtml() returns a complete HTML document even for an
    // empty Markdown source. Keep QLabel::text() genuinely empty here because
    // ChatArea uses text().isEmpty() to decide whether a DM presence string
    // (online/offline/etc.) still needs to be installed.
    if (text.isEmpty()) {
        formattedText.clear();
        QLabel::setText(QString());
        updateCollapsedHeight();
        hidePopover();
        if (popover) {
            popover->clear();
        }
        return;
    }

    formattedText = MessageFormatter::formatMessageText(text);
    refreshRenderedText();
    updateCollapsedHeight();

    if (popover && popover->isVisible()) {
        if (isOverflowing()) {
            positionPopover();
        } else {
            hidePopover();
        }
    }
}

void ChannelHeaderTextLabel::changeEvent(QEvent* event)
{
    QLabel::changeEvent(event);
    if (!event) {
        return;
    }

    if (event->type() == QEvent::PaletteChange
        || event->type() == QEvent::ApplicationPaletteChange
        || event->type() == QEvent::StyleChange) {
        // QLabel caches the QTextDocument created for rich text. The widget
        // palette itself changes correctly when the desktop theme switches, but
        // an already parsed document can keep drawing with the old foreground
        // until the text changes. Force a reparse after QLabel has accepted the
        // new palette so DM presence/header text follows the theme immediately.
        refreshRenderedText();
    }
}

void ChannelHeaderTextLabel::refreshRenderedText()
{
    if (sourceText.isEmpty()) {
        return;
    }

    // QLabel::setText() can short-circuit when the HTML string is unchanged.
    // Clear only the base-class rendering state (not sourceText/formattedText)
    // so the rich-text control is definitely recreated with the current palette.
    QLabel::setText(QString());
    QLabel::setText(formattedText);

    if (popover) {
        popover->setHtml(formattedText);
    }
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
    QWidget* host = window();
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
    browser->setOpenExternalLinks(true);
    browser->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::TextSelectableByMouse);
    browser->setFrameShape(QFrame::Box);
    browser->setFrameShadow(QFrame::Plain);
    browser->setLineWidth(1);
    browser->setAutoFillBackground(true);
    browser->setStyleSheet(QStringLiteral(
        "QTextBrowser#channelHeaderTextPopover {"
        " border: 1px solid palette(mid);"
        " background: palette(base);"
        " }"));
    browser->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    browser->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    browser->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    browser->document()->setDocumentMargin(4);
    browser->setHtml(formattedText);
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

    const QPoint labelPos = mapTo(host, QPoint(0, 0));
    const int left = std::max(4, labelPos.x() - 4);
    const int availableWidth = std::max(120, host->width() - left - 4);

    // Measure with a throw-away document instead of the QTextBrowser's current
    // viewport. On the first hover QTextBrowser can still carry the default
    // pre-show viewport width, which made document()->size() report a wildly
    // inflated height. The second hover happened after layout and therefore
    // appeared correct. A standalone document makes both passes deterministic.
    QTextDocument measure;
    measure.setDefaultFont(popover->font());
    measure.setDocumentMargin(4);
    measure.setHtml(formattedText);

    const int naturalWidth = static_cast<int>(std::ceil(measure.idealWidth())) + 10;
    const int labelWidth = std::max(1, width() + 8);
    const int wantedWidth = std::max(220, std::min(naturalWidth, labelWidth));
    const int popupWidth = std::min(wantedWidth, availableWidth);

    measure.setTextWidth(std::max(1, popupWidth - 10));
    const int documentHeight = static_cast<int>(std::ceil(measure.size().height())) + 10;

    // Start on top of the compact line and expand downwards. The overlap avoids
    // a mouse gap between the reference label and the interactive overlay.
    const int top = std::max(4, labelPos.y() - 4);
    const int availableHeight = std::max(80, host->height() - top - 4);
    const int popupHeight = std::min(std::max(height() + 8, documentHeight), availableHeight);

    // Apply the final text width before showing the browser so its first layout
    // already matches the geometry we just measured.
    popover->document()->setTextWidth(std::max(1, popupWidth - 10));
    popover->setGeometry(left, top, popupWidth, popupHeight);
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

bool ChannelHeaderTextLabel::eventFilter(QObject* watched, QEvent* event)
{
    const bool isLabel = watched == this;
    const bool isPopover = popover && (watched == popover.data() || watched == popover->viewport());

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
            }
            break;
        default:
            break;
        }
    }

    return QLabel::eventFilter(watched, event);
}

} // namespace Mattermost
