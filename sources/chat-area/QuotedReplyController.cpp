#include "QuotedReplyController.h"

#include <QByteArray>
#include <QEvent>
#include <QHash>
#include <QHBoxLayout>
#include <QPointer>
#include <QSizePolicy>
#include <QToolButton>
#include <QVariant>

#include "ChatArea.h"
#include "QuotedPostPreview.h"
#include "backend/PostProps.h"
#include "backend/types/BackendPost.h"
#include "chat-area/outgoing-post/OutgoingPostCreator.h"
#include "ui_ChatArea.h"

namespace Mattermost {
namespace {

constexpr char EditingPostProperty[] = "_mmqt_editing_post";

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
    if (preview || !area.ui || !area.ui->outgoingPostCreator
        || !area.ui->composerInputContainer || !area.ui->composerInputLayout) {
        return;
    }

    editor = area.ui->outgoingPostCreator;
    wrapper = area.ui->composerInputContainer;

    previewRow = new QWidget(wrapper);
    previewRow->setObjectName(QStringLiteral("quotedReplyPreviewRow"));
    previewRow->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);

    auto* rowLayout = new QHBoxLayout(previewRow);
    rowLayout->setContentsMargins(2, 2, 2, 1);
    rowLayout->setSpacing(4);

    preview = new QuotedPostPreview(previewRow, 2);
    rowLayout->addWidget(preview, 1);

    cancelButton = new QToolButton(previewRow);
    cancelButton->setText(QString::fromUtf8("×"));
    cancelButton->setAutoRaise(true);
    cancelButton->setCursor(Qt::PointingHandCursor);
    cancelButton->setToolTip(tr("Cancel reply"));
    cancelButton->setAccessibleName(tr("Cancel reply"));
    rowLayout->addWidget(cancelButton, 0, Qt::AlignTop);

    area.ui->composerInputLayout->insertWidget(0, previewRow);

    previewRow->hide();
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

    preview->setPost(post);
    editor->setProperty(PostProps::ReplyToPostId, post.id);
    previewRow->show();
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
    if (!editor || !previewRow) {
        return;
    }

    const bool hasReply = !editor->property(PostProps::ReplyToPostId).toString().isEmpty();
    previewRow->setVisible(hasReply);
    if (wrapper) {
        wrapper->updateGeometry();
    }
}

} // namespace Mattermost
