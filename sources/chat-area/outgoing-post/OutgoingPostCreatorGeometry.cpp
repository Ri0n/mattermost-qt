#include "OutgoingPostCreator.h"

#include <QEvent>
#include <QLayout>

#include "MessageTextEditWidget.h"
#include "ui_OutgoingPostCreator.h"

namespace Mattermost {

int OutgoingPostCreator::editorHeightHint() const
{
    if (!ui || !ui->textEdit) {
        return QWidget::sizeHint().height();
    }

    const QMargins margins = layout() ? layout()->contentsMargins() : QMargins();
    return ui->textEdit->height() + margins.top() + margins.bottom();
}

QSize OutgoingPostCreator::sizeHint() const
{
    QSize hint = QWidget::sizeHint();
    hint.setHeight(editorHeightHint());
    return hint;
}

bool OutgoingPostCreator::event(QEvent* event)
{
    const bool handled = QWidget::event(event);
    if (!event || event->type() != QEvent::LayoutRequest) {
        return handled;
    }

    const int wantedHeight = editorHeightHint();
    if (minimumHeight() != wantedHeight || maximumHeight() != wantedHeight) {
        setMinimumHeight(wantedHeight);
        setMaximumHeight(wantedHeight);
        updateGeometry();
    }

    return handled;
}

} // namespace Mattermost
