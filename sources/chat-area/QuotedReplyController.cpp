#include "QuotedReplyController.h"

#include <QBoxLayout>
#include <QByteArray>
#include <QEvent>
#include <QFont>
#include <QFrame>
#include <QHash>
#include <QLabel>
#include <QPointer>
#include <QSizePolicy>
#include <QToolButton>
#include <QVariant>
#include <QVBoxLayout>

#include "ChatArea.h"
#include "backend/PostProps.h"
#include "backend/types/BackendPost.h"
#include "chat-area/outgoing-post/OutgoingPostCreator.h"
#include "ui_ChatArea.h"

namespace Mattermost {
namespace {

constexpr char EditingPostProperty[] = "_mmqt_editing_post";
constexpr int ReplyPreviewTextLimit = 180;

QString previewText(const BackendPost& post)
{
    QString text = post.message;
    text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    text = text.simplified();

    if (text.isEmpty() && !post.files.empty()) {
        text = QObject::tr("[attachment]");
    } else if (text.isEmpty()) {
        text = QObject::tr("[empty message]");
    }

    if (text.size() > ReplyPreviewTextLimit) {
        text.truncate(ReplyPreviewTextLimit - 1);
        text += QChar(0x2026);
    }
    return text;
}

} // namespace

QuotedReplyController& QuotedReplyController::instance(ChatArea& area)
{
    static QHash<ChatArea*, QPointer<QuotedReplyController>> instances;
    QPointer<QuotedReplyController>& controller = instances[&area];
    if (!controller) {
        controller = new QuotedReplyController(area);
    }
    return *controller;
}

QuotedReplyController::QuotedReplyController(ChatArea& sourceArea)
    : QObject(&sourceArea)
    , area(sourceArea)
{
    ensureUi();
}

void QuotedReplyController::ensureUi()
{
    if (wrapper || !area.ui || !area.ui->outgoingPostCreator || !area.ui->composerLayout) {
        return;
    }

    editor = area.ui->outgoingPostCreator;
    const int editorIndex = area.ui->composerLayout->indexOf(editor);
    if (editorIndex < 0) {
        return;
    }

    area.ui->composerLayout->removeWidget(editor);

    wrapper = new QWidget(&area);
    wrapper->setObjectName(QStringLiteral("composerInputWrapper"));
    wrapper->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    auto* wrapperLayout = new QVBoxLayout(wrapper);
    wrapperLayout->setContentsMargins(0, 0, 0, 0);
    wrapperLayout->setSpacing(2);

    preview = new QFrame(wrapper);
    preview->setObjectName(QStringLiteral("quotedReplyPreview"));
    preview->setFrameShape(QFrame::StyledPanel);
    preview->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);

    auto* previewLayout = new QHBoxLayout(preview);
    previewLayout->setContentsMargins(6, 3, 3, 3);
    previewLayout->setSpacing(6);

    auto* textLayout = new QVBoxLayout;
    textLayout->setContentsMargins(0, 0, 0, 0);
    textLayout->setSpacing(0);

    authorLabel = new QLabel(preview);
    QFont authorFont = authorLabel->font();
    authorFont.setBold(true);
    authorLabel->setFont(authorFont);
    authorLabel->setTextFormat(Qt::PlainText);

    messageLabel = new QLabel(preview);
    messageLabel->setTextFormat(Qt::PlainText);
    messageLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    textLayout->addWidget(authorLabel);
    textLayout->addWidget(messageLabel);
    previewLayout->addLayout(textLayout, 1);

    cancelButton = new QToolButton(preview);
    cancelButton->setText(QString::fromUtf8("×"));
    cancelButton->setAutoRaise(true);
    cancelButton->setCursor(Qt::PointingHandCursor);
    cancelButton->setToolTip(tr("Cancel reply"));
    cancelButton->setAccessibleName(tr("Cancel reply"));
    previewLayout->addWidget(cancelButton, 0, Qt::AlignTop);

    wrapperLayout->addWidget(preview);
    wrapperLayout->addWidget(editor);
    area.ui->composerLayout->insertWidget(editorIndex, wrapper, 1);

    preview->hide();
    connect(cancelButton, &QToolButton::clicked, this, &QuotedReplyController::cancel);
    editor->installEventFilter(this);
    syncVisibility();
}

void QuotedReplyController::begin(const BackendPost& post)
{
    ensureUi();
    if (!editor || !preview || editor->isReadOnly()
        || editor->property(EditingPostProperty).toBool()) {
        return;
    }

    authorLabel->setText(tr("Replying to %1").arg(post.getDisplayAuthorName()));
    const QString text = previewText(post);
    messageLabel->setText(text);
    messageLabel->setToolTip(post.message);

    editor->setProperty(PostProps::ReplyToPostId, post.id);
    preview->show();
    wrapper->updateGeometry();
    editor->setFocus(Qt::OtherFocusReason);
}

void QuotedReplyController::cancel()
{
    if (!editor) {
        return;
    }
    editor->setProperty(PostProps::ReplyToPostId, QString());
    syncVisibility();
}

bool QuotedReplyController::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == editor && event && event->type() == QEvent::DynamicPropertyChange) {
        auto* propertyEvent = static_cast<QDynamicPropertyChangeEvent*>(event);
        if (propertyEvent->propertyName() == QByteArray(PostProps::ReplyToPostId)) {
            syncVisibility();
        }
    }
    return QObject::eventFilter(watched, event);
}

void QuotedReplyController::syncVisibility()
{
    if (!editor || !preview) {
        return;
    }

    const bool hasReply = !editor->property(PostProps::ReplyToPostId).toString().isEmpty();
    preview->setVisible(hasReply);
    if (wrapper) {
        wrapper->updateGeometry();
    }
}

} // namespace Mattermost
