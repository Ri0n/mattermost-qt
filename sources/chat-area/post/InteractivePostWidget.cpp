#include "InteractivePostWidget.h"

#include <algorithm>

#include <QEasingCurve>
#include <QEvent>
#include <QPainter>
#include <QPalette>
#include <QVariantAnimation>

namespace Mattermost {

namespace {

constexpr int HoverAlpha = 24;
constexpr int NavigationDurationMs = 3000;

} // namespace

InteractivePostWidget::InteractivePostWidget(Backend& backend,
                                             BackendPost& post,
                                             QWidget* parent,
                                             ChatArea* chatArea,
                                             BackendPost* lastRootPost)
    : PostWidget(backend, post, parent, chatArea, lastRootPost)
{
    setMouseTracking(true);
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

bool InteractivePostWidget::event(QEvent* event)
{
    if (event) {
        if (event->type() == QEvent::Enter && !hovered) {
            hovered = true;
            update();
        } else if (event->type() == QEvent::Leave && hovered) {
            hovered = false;
            update();
        }
    }
    return PostWidget::event(event);
}

void InteractivePostWidget::paintEvent(QPaintEvent* event)
{
    PostWidget::paintEvent(event);

    const int alpha = std::max(navigationAlpha, hovered ? HoverAlpha : 0);
    if (alpha <= 0) {
        return;
    }

    QColor background = palette().color(QPalette::Highlight);
    background.setAlpha(alpha);
    QPainter painter(this);
    painter.fillRect(rect(), background);
}

} // namespace Mattermost
