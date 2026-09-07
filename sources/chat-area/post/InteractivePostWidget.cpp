#include "InteractivePostWidget.h"

#include <QEasingCurve>
#include <QPainter>
#include <QPalette>
#include <QVariantAnimation>

namespace Mattermost {

namespace {

constexpr int NavigationDurationMs = 3000;

} // namespace

InteractivePostWidget::InteractivePostWidget(Backend& backend,
                                             BackendPost& post,
                                             QWidget* parent,
                                             ChatArea* chatArea,
                                             BackendPost* lastRootPost)
    : PostWidget(backend, post, parent, chatArea, lastRootPost)
{
}

void InteractivePostWidget::animateNavigationHighlight()
{
    if (!navigationAnimation) {
        navigationAnimation = new QVariantAnimation(this);
        navigationAnimation->setDuration(NavigationDurationMs);
        navigationAnimation->setStartValue(0);
        navigationAnimation->setKeyValueAt(0.08, 120);
        navigationAnimation->setKeyValueAt(0.27, 35);
        navigationAnimation->setKeyValueAt(0.43, 100);
        navigationAnimation->setKeyValueAt(0.62, 20);
        navigationAnimation->setKeyValueAt(0.78, 70);
        navigationAnimation->setEndValue(0);
        navigationAnimation->setEasingCurve(QEasingCurve::InOutSine);

        connect(navigationAnimation, &QVariantAnimation::valueChanged,
                this, [this](const QVariant& value) {
            navigationAlpha = value.toInt();
            update();
        });
        connect(navigationAnimation, &QVariantAnimation::finished,
                this, [this] {
            navigationAlpha = 0;
            update();
        });
    }

    navigationAnimation->stop();
    navigationAlpha = 0;
    navigationAnimation->start();
}

void InteractivePostWidget::paintEvent(QPaintEvent* event)
{
    PostWidget::paintEvent(event);

    if (navigationAlpha <= 0) {
        return;
    }

    QColor background = palette().color(QPalette::Highlight);
    background.setAlpha(navigationAlpha);
    QPainter painter(this);
    painter.fillRect(rect(), background);
}

} // namespace Mattermost
