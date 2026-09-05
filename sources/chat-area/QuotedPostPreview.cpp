#include "QuotedPostPreview.h"

#include <algorithm>

#include <QContextMenuEvent>
#include <QCoreApplication>
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
    bar->setBackgroundRole(QPalette::Mid);
    bar->setAutoFillBackground(true);
    bar->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    bar->installEventFilter(this);
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
    authorLabel->installEventFilter(this);

    messageLabel = new QLabel(this);
    messageLabel->setTextFormat(Qt::PlainText);
    messageLabel->setWordWrap(maximumLines > 1);
    messageLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    messageLabel->setMaximumHeight(
        messageLabel->fontMetrics().lineSpacing() * maximumLines + 2);
    messageLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    messageLabel->installEventFilter(this);

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
                  || event->type() == QEvent::ApplicationPaletteChange
                  || event->type() == QEvent::StyleChange)) {
        refreshPalette();
    }
}

void QuotedPostPreview::contextMenuEvent(QContextMenuEvent* event)
{
    if (!event || !parentWidget()) {
        QFrame::contextMenuEvent(event);
        return;
    }

    QContextMenuEvent forwarded(event->reason(),
                                parentWidget()->mapFromGlobal(event->globalPos()),
                                event->globalPos(),
                                event->modifiers());
    QCoreApplication::sendEvent(parentWidget(), &forwarded);
    if (forwarded.isAccepted()) {
        event->accept();
    } else {
        event->ignore();
    }
}

bool QuotedPostPreview::eventFilter(QObject* watched, QEvent* event)
{
    if (event && event->type() == QEvent::ContextMenu
        && (watched == authorLabel || watched == messageLabel || watched == bar)) {
        auto* contextEvent = static_cast<QContextMenuEvent*>(event);
        contextMenuEvent(contextEvent);
        return contextEvent->isAccepted();
    }
    return QFrame::eventFilter(watched, event);
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
