#include <QApplication>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QEvent>
#include <QMouseEvent>
#include <QObject>
#include <QPointer>
#include <QWidget>

#include "PostWidget.h"
#include "chat-area/ChatLogWidget.h"

namespace Mattermost {
namespace {

PostWidget* enclosingPost(QWidget* widget)
{
    for (QWidget* current = widget; current; current = current->parentWidget()) {
        if (auto* post = qobject_cast<PostWidget*>(current)) {
            return post;
        }
    }
    return nullptr;
}

ChatLogWidget* enclosingLog(QWidget* widget)
{
    for (QWidget* current = widget; current; current = current->parentWidget()) {
        if (auto* log = qobject_cast<ChatLogWidget*>(current)) {
            return log;
        }
    }
    return nullptr;
}

QPoint globalMousePosition(const QMouseEvent* event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->globalPosition().toPoint();
#else
    return event->globalPos();
#endif
}

class PostContextMenuRouter final : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (!event) {
            return QObject::eventFilter(watched, event);
        }

        QWidget* widget = qobject_cast<QWidget*>(watched);
        if (!widget) {
            return QObject::eventFilter(watched, event);
        }

        if (event->type() == QEvent::ContextMenu) {
            PostWidget* postWidget = enclosingPost(widget);
            if (!postWidget || widget == postWidget) {
                return QObject::eventFilter(watched, event);
            }
            auto* contextEvent = static_cast<QContextMenuEvent*>(event);
            const QPoint postPosition = postWidget->mapFromGlobal(contextEvent->globalPos());
            QContextMenuEvent forwarded(contextEvent->reason(), postPosition,
                                        contextEvent->globalPos(), contextEvent->modifiers());
            QCoreApplication::sendEvent(postWidget, &forwarded);
            contextEvent->accept();
            return true;
        }

        if (event->type() == QEvent::MouseButtonPress) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton) {
                PostWidget* post = enclosingPost(widget);
                ChatLogWidget* log = post ? enclosingLog(post) : nullptr;
                if (post && log && !log->isMessageSelectionMode()) {
                    dragOrigin_ = post;
                    dragLog_ = log;
                    wholePostDrag_ = false;
                } else {
                    clearDrag();
                }
            }
            return QObject::eventFilter(watched, event);
        }

        if (event->type() == QEvent::MouseMove && dragOrigin_ && dragLog_) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (!(mouse->buttons() & Qt::LeftButton)) {
                clearDrag();
                return QObject::eventFilter(watched, event);
            }

            const QPoint viewportPos = dragLog_->viewport()->mapFromGlobal(globalMousePosition(mouse));
            const int index = dragLog_->indexAtViewportPosition(viewportPos.y());
            auto* current = index >= 0
                ? qobject_cast<PostWidget*>(dragLog_->itemWidget(index)) : nullptr;
            if (!current) {
                return wholePostDrag_ ? true : QObject::eventFilter(watched, event);
            }

            if (!wholePostDrag_) {
                if (current == dragOrigin_ || dragOrigin_->getSelectedText().isEmpty()) {
                    return QObject::eventFilter(watched, event);
                }
                wholePostDrag_ = true;
                dragOrigin_->clearTextSelection();
                dragLog_->beginMessageSelectionDrag(dragOrigin_->post.id, current->post.id);
                return true;
            }

            dragLog_->updateMessageSelectionDrag(current->post.id);
            return true;
        }

        if (event->type() == QEvent::MouseButtonRelease) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton && dragOrigin_) {
                const bool consumed = wholePostDrag_;
                if (wholePostDrag_ && dragLog_) {
                    dragLog_->finishMessageSelectionDrag();
                }
                clearDrag();
                if (consumed) {
                    return true;
                }
            }
        }

        return QObject::eventFilter(watched, event);
    }

private:
    void clearDrag()
    {
        dragOrigin_.clear();
        dragLog_.clear();
        wholePostDrag_ = false;
    }

    QPointer<PostWidget> dragOrigin_;
    QPointer<ChatLogWidget> dragLog_;
    bool wholePostDrag_ = false;
};

void installPostContextMenuRouter()
{
    if (!qApp) {
        return;
    }
    auto* router = new PostContextMenuRouter(qApp);
    qApp->installEventFilter(router);
}

Q_COREAPP_STARTUP_FUNCTION(installPostContextMenuRouter)

} // namespace
} // namespace Mattermost
