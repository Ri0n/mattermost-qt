#include "QuotedPostPreview.h"

#include <algorithm>

#include <QEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPalette>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QVBoxLayout>

#include "QuotedReplyFormat.h"
#include "backend/types/BackendPost.h"

namespace Mattermost {

QuotedPostPreview::QuotedPostPreview(QWidget* parent, int maximumLinesValue)
    : QFrame(parent)
    , maximumLines(std::max(1, maximumLinesValue))
{
    setFrameShape(QFrame::NoFrame);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 1, 0, 1);
    layout->setSpacing(7);

    bar = new QFrame(this);
    bar->setFixedWidth(3);
    bar->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    bar->setAutoFillBackground(true);
    layout->addWidget(bar);

    auto* textLayout = new QVBoxLayout;
    textLayout->setContentsMargins(0, 0, 0, 0);
    textLayout->setSpacing(0);

    authorLabel = new QLabel(this);
    QFont authorFont = authorLabel->font();
    authorFont.setBold(true);
    authorLabel->setFont(authorFont);
    authorLabel->setTextFormat(Qt::PlainText);
    authorLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    messageLabel = new QLabel(this);
    messageLabel->setTextFormat(Qt::PlainText);
    messageLabel->setWordWrap(maximumLines > 1);
    messageLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    messageLabel->setMaximumHeight(
        messageLabel->fontMetrics().lineSpacing() * maximumLines + 2);

    textLayout->addWidget(authorLabel);
    textLayout->addWidget(messageLabel);
    layout->addLayout(textLayout, 1);

    refreshPalette();
}

void QuotedPostPreview::setPost(const BackendPost& post)
{
    authorLabel->setText(QObject::tr("Replying to %1").arg(post.getDisplayAuthorName()));
    fullText = QuotedReplyFormat::compactText(
        QuotedReplyFormat::stripFallback(post.message), !post.files.empty(), 500);
    setToolTip(QuotedReplyFormat::stripFallback(post.message));
    refreshText();
}

void QuotedPostPreview::setActivatedCallback(std::function<void()> callback)
{
    activatedCallback = std::move(callback);
    setCursor(activatedCallback ? Qt::PointingHandCursor : Qt::ArrowCursor);
}

void QuotedPostPreview::changeEvent(QEvent* event)
{
    QFrame::changeEvent(event);
    if (event && (event->type() == QEvent::PaletteChange
                  || event->type() == QEvent::ApplicationPaletteChange)) {
        refreshPalette();
    }
}

void QuotedPostPreview::mouseReleaseEvent(QMouseEvent* event)
{
    if (event && event->button() == Qt::LeftButton && activatedCallback) {
        activatedCallback();
        event->accept();
        return;
    }
    QFrame::mouseReleaseEvent(event);
}

void QuotedPostPreview::resizeEvent(QResizeEvent* event)
{
    QFrame::resizeEvent(event);
    refreshText();
}

void QuotedPostPreview::refreshPalette()
{
    const QPalette source = palette();

    QPalette barPalette = bar->palette();
    barPalette.setColor(QPalette::Window, source.color(QPalette::Mid));
    bar->setPalette(barPalette);

    // PlaceholderText is the palette's semantic secondary/muted foreground.
    // Disabled/Text is not suitable here: styles are allowed to make it equal
    // to normal WindowText, which made log quotes look like primary content.
    const QColor muted = source.color(QPalette::PlaceholderText);
    QPalette authorPalette = authorLabel->palette();
    authorPalette.setColor(QPalette::WindowText, muted);
    authorLabel->setPalette(authorPalette);

    QPalette messagePalette = messageLabel->palette();
    messagePalette.setColor(QPalette::WindowText, muted);
    messageLabel->setPalette(messagePalette);
}

void QuotedPostPreview::refreshText()
{
    if (!messageLabel || fullText.isEmpty()) {
        if (messageLabel) {
            messageLabel->clear();
        }
        return;
    }

    const int availableWidth = std::max(80, messageLabel->width());
    const int averageCharWidth = std::max(1, messageLabel->fontMetrics().averageCharWidth());
    const int estimatedCharacters = std::max(
        24, maximumLines * availableWidth / averageCharWidth);

    QString text = fullText;
    if (text.size() > estimatedCharacters) {
        text.truncate(std::max(1, estimatedCharacters - 1));
        text += QChar(0x2026);
    }
    messageLabel->setText(text);
}

} // namespace Mattermost
