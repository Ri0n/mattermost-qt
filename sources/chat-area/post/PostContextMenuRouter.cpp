#include <QApplication>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QEvent>
#include <QObject>
#include <QWidget>

#include "PostWidget.h"

namespace Mattermost {
namespace {

class PostContextMenuRouter final : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (!event || event->type() != QEvent::ContextMenu) {
            return QObject::eventFilter(watched, event);
        }

        QWidget* widget = qobject_cast<QWidget*>(watched);
        if (!widget) {
            return QObject::eventFilter(watched, event);
        }

        PostWidget* postWidget = nullptr;
        for (QWidget* current = widget; current; current = current->parentWidget()) {
            if (auto* post = qobject_cast<PostWidget*>(current)) {
                postWidget = post;
                break;
            }
        }

        if (!postWidget || widget == postWidget) {
            return QObject::eventFilter(watched, event);
        }

        auto* contextEvent = static_cast<QContextMenuEvent*>(event);
        const QPoint postPosition = postWidget->mapFromGlobal(contextEvent->globalPos());
        QContextMenuEvent forwarded(contextEvent->reason(),
                                    postPosition,
                                    contextEvent->globalPos(),
                                    contextEvent->modifiers());
        QCoreApplication::sendEvent(postWidget, &forwarded);
        contextEvent->accept();
        return true;
    }
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
