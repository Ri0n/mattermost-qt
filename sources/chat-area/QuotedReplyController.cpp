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
    previewRow->setObjectName(QStringLiteral("composerContextPreviewRow"));
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

    mode = Mode::Reply;
    preview->setActivatedCallback({});
    preview->setPost(post);
    editor->setProperty(PostProps::ReplyToPostId, post.id);
    syncVisibility();
    editor->setFocus(Qt::OtherFocusReason);
}

void QuotedReplyController::cancel()
{
    if (!editor) {
        return;
    }

    if (mode == Mode::Editing) {
        editor->cancelPostEdit();
        return;
    }

    editor->setProperty(PostProps::ReplyToPostId, QString());
    if (mode == Mode::Reply) {
        mode = Mode::None;
    }
    syncVisibility();
}

bool QuotedReplyController::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != editor || !event || event->type() != QEvent::DynamicPropertyChange) {
        return QObject::eventFilter(watched, event);
    }

    auto* propertyEvent = static_cast<QDynamicPropertyChangeEvent*>(event);
    const QByteArray name = propertyEvent->propertyName();

    if (name == QByteArray(PostProps::ReplyToPostId)) {
        const bool hasReply = !editor->property(PostProps::ReplyToPostId).toString().isEmpty();
        if (!hasReply && mode == Mode::Reply) {
            mode = Mode::None;
        }
        syncVisibility();
    } else if (name == QByteArray(EditingPostProperty)) {
        const bool editing = editor->property(EditingPostProperty).toBool();
        if (editing) {
            mode = Mode::Editing;
            preview->setActivatedCallback({});
            preview->setPreview(tr("Editing message"), editor->toPlainText());
        } else if (mode == Mode::Editing) {
            mode = Mode::None;
        }
        syncVisibility();
    }

    return QObject::eventFilter(watched, event);
}

void QuotedReplyController::syncVisibility()
{
    if (!editor || !previewRow) {
        return;
    }

    const bool hasReply = !editor->property(PostProps::ReplyToPostId).toString().isEmpty();
    const bool editing = editor->property(EditingPostProperty).toBool();
    const bool visible = (mode == Mode::Reply && hasReply)
        || (mode == Mode::Editing && editing);

    if (cancelButton) {
        if (mode == Mode::Editing) {
            cancelButton->setToolTip(tr("Cancel editing"));
            cancelButton->setAccessibleName(tr("Cancel editing"));
            cancelButton->setEnabled(!editor->isReadOnly());
        } else {
            cancelButton->setToolTip(tr("Cancel reply"));
            cancelButton->setAccessibleName(tr("Cancel reply"));
            cancelButton->setEnabled(true);
        }
    }

    previewRow->setVisible(visible);
    if (wrapper) {
        wrapper->updateGeometry();
    }
}

} // namespace Mattermost
