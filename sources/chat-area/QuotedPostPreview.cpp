#include "QuotedPostPreview.h"

#include <algorithm>

#include <QEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPalette>
#include <QSizePolicy>
#include <QVBoxLayout>

#include "QuotedReplyFormat.h"
#include "backend/types/BackendPost.h"
#include "post/MessageFormatter.h"

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
    bar->setBackgroundRole(QPalette::Mid);
    bar->setAutoFillBackground(true);
    bar->setAttribute(Qt::WA_TransparentForMouseEvents, true);
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
    authorLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);

    messageLabel = new QLabel(this);
    messageLabel->setTextFormat(Qt::RichText);
    messageLabel->setWordWrap(maximumLines > 1);
    messageLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    messageLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    messageLabel->setMaximumHeight(
        messageLabel->fontMetrics().lineSpacing() * maximumLines + 2);
    messageLabel->setTextInteractionFlags(Qt::NoTextInteraction);
    messageLabel->setOpenExternalLinks(false);
    messageLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);

    textLayout->addWidget(authorLabel);
    textLayout->addWidget(messageLabel);
    layout->addLayout(textLayout, 1);

    refreshPalette();
}

void QuotedPostPreview::setPost(const BackendPost& post)
{
    setPreview(QObject::tr("Replying to %1").arg(post.getDisplayAuthorName()),
               post.message,
               !post.files.empty());
}

void QuotedPostPreview::setPreview(const QString& title,
                                   const QString& message,
                                   bool hasAttachments)
{
    authorLabel->setText(title);
    const QString visibleMessage = QuotedReplyFormat::stripFallback(message);
    if (visibleMessage.trimmed().isEmpty()) {
        fullText = hasAttachments
            ? QStringLiteral("[attachment]")
            : QStringLiteral("[empty message]");
    } else {
        fullText = visibleMessage;
    }
    setToolTip(visibleMessage);
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
                  || event->type() == QEvent::ApplicationPaletteChange
                  || event->type() == QEvent::StyleChange
                  || event->type() == QEvent::FontChange)) {
        if (messageLabel) {
            messageLabel->setMaximumHeight(
                messageLabel->fontMetrics().lineSpacing() * maximumLines + 2);
        }
        refreshPalette();
        refreshText();
    }
}

void QuotedPostPreview::mousePressEvent(QMouseEvent* event)
{
    if (event && event->button() == Qt::LeftButton && activatedCallback) {
        activatedCallback();
        event->accept();
        return;
    }
    QFrame::mousePressEvent(event);
}

void QuotedPostPreview::refreshPalette()
{
    if (!authorLabel || !messageLabel) {
        return;
    }

    const QColor textColor = palette().color(QPalette::Text);
    QColor mutedColor = palette().color(QPalette::PlaceholderText);

    if (!mutedColor.isValid() || mutedColor.rgba() == textColor.rgba()) {
        mutedColor = textColor;
        mutedColor.setAlphaF(mutedColor.alphaF() * 0.65);
    }

    QPalette mutedPalette = palette();
    mutedPalette.setColor(QPalette::WindowText, mutedColor);
    authorLabel->setForegroundRole(QPalette::WindowText);
    messageLabel->setForegroundRole(QPalette::WindowText);
    authorLabel->setPalette(mutedPalette);
    messageLabel->setPalette(mutedPalette);
}

void QuotedPostPreview::refreshText()
{
    if (!messageLabel || fullText.isEmpty()) {
        if (messageLabel) {
            messageLabel->clear();
        }
        return;
    }

    messageLabel->setText(MessageFormatter::formatMessageText(fullText));
}

} // namespace Mattermost
