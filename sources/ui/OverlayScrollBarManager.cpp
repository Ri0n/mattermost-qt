/*
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Mattermost-QT is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 */

#include "OverlayScrollBarManager.h"

#include <algorithm>
#include <cmath>

#include <QAbstractScrollArea>
#include <QApplication>
#include <QColor>
#include <QCursor>
#include <QEasingCurve>
#include <QEvent>
#include <QGraphicsOpacityEffect>
#include <QPalette>
#include <QPropertyAnimation>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTimer>
#include <QWidget>

namespace Mattermost {
namespace {

// Keep the painted thumb narrow, but make its actual mouse target considerably
// more forgiving. The outside pixel remains visually empty while still
// belonging to the scrollbar, so the pointer cannot slip through at the window
// edge. The inner hit padding is roughly half of the visible thumb thickness.
constexpr int VisibleThumbThickness = 5;
constexpr int OuterHitPadding = 1;
constexpr int InnerHitPadding = (VisibleThumbThickness + 1) / 2;
constexpr int ScrollBarHitThickness = InnerHitPadding + VisibleThumbThickness + OuterHitPadding;
constexpr int ScrollBarEndInset = 1;
constexpr int FadeDelayMs = 900;
constexpr int FadeDurationMs = 240;
constexpr int RevealDurationMs = 90;
constexpr char InstalledProperty[] = "mattermostOverlayScrollBarsInstalled";
constexpr char VerticalObjectName[] = "mattermostOverlayVerticalScrollBar";
constexpr char HorizontalObjectName[] = "mattermostOverlayHorizontalScrollBar";

QString cssRgba(const QColor& color)
{
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alpha());
}

QString overlayStyleSheet(const QColor& handle)
{
    return QStringLiteral(
        "QScrollBar:vertical {"
        " background: transparent; border: 0; width: %2px; margin: 0;"
        "}"
        "QScrollBar::handle:vertical {"
        " background: %1; border: 0; border-radius: 2px;"
        " min-height: 28px; margin: 1px %3px 1px %4px;"
        "}"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {"
        " height: 0; border: 0; background: transparent;"
        "}"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {"
        " background: transparent;"
        "}"
        "QScrollBar:horizontal {"
        " background: transparent; border: 0; height: %2px; margin: 0;"
        "}"
        "QScrollBar::handle:horizontal {"
        " background: %1; border: 0; border-radius: 2px;"
        " min-width: 28px; margin: %4px 1px %3px 1px;"
        "}"
        "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {"
        " width: 0; border: 0; background: transparent;"
        "}"
        "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {"
        " background: transparent;"
        "}")
        .arg(cssRgba(handle))
        .arg(ScrollBarHitThickness)
        .arg(OuterHitPadding)
        .arg(InnerHitPadding);
}

QScrollBar* createOverlay(QAbstractScrollArea& area,
                          Qt::Orientation orientation,
                          const char* objectName)
{
    auto* bar = new QScrollBar(orientation, &area);
    bar->setObjectName(QString::fromLatin1(objectName));
    bar->setFocusPolicy(Qt::NoFocus);
    bar->setMouseTracking(true);
    bar->hide();
    return bar;
}

bool scrollable(const QScrollBar* bar)
{
    return bar && bar->maximum() > bar->minimum();
}

void syncBar(QScrollBar* source, QScrollBar* overlay)
{
    if (!source || !overlay) {
        return;
    }

    const QSignalBlocker blocker(overlay);
    overlay->setRange(source->minimum(), source->maximum());
    overlay->setPageStep(source->pageStep());
    overlay->setSingleStep(source->singleStep());
    overlay->setTracking(source->hasTracking());
    overlay->setInvertedAppearance(source->invertedAppearance());
    overlay->setInvertedControls(source->invertedControls());
    overlay->setValue(source->value());
}

void animateOpacity(QGraphicsOpacityEffect* effect,
                    QPropertyAnimation* animation,
                    qreal target,
                    int duration)
{
    if (!effect || !animation) {
        return;
    }
    if (std::abs(effect->opacity() - target) < 0.01) {
        effect->setOpacity(target);
        return;
    }

    animation->stop();
    animation->setDuration(duration);
    animation->setStartValue(effect->opacity());
    animation->setEndValue(target);
    animation->start();
}

} // namespace

struct OverlayScrollBarManager::State {
    QAbstractScrollArea* area = nullptr;
    QScrollBar* sourceVertical = nullptr;
    QScrollBar* sourceHorizontal = nullptr;
    QScrollBar* overlayVertical = nullptr;
    QScrollBar* overlayHorizontal = nullptr;
    QGraphicsOpacityEffect* verticalOpacity = nullptr;
    QGraphicsOpacityEffect* horizontalOpacity = nullptr;
    QPropertyAnimation* verticalAnimation = nullptr;
    QPropertyAnimation* horizontalAnimation = nullptr;
    QTimer* fadeTimer = nullptr;
    bool verticalEnabled = false;
    bool horizontalEnabled = false;
    bool cursorInside = false;
};

void OverlayScrollBarManager::install(QApplication& application)
{
    if (application.property(InstalledProperty).toBool()) {
        return;
    }
    application.setProperty(InstalledProperty, true);
    new OverlayScrollBarManager(application);
}

OverlayScrollBarManager::OverlayScrollBarManager(QApplication& application)
    : QObject(&application)
{
    application.installEventFilter(this);

    // Usually the manager is installed before any windows are constructed, but
    // keep late installation deterministic for tests and embedded use.
    for (QWidget* widget : QApplication::allWidgets()) {
        if (auto* area = qobject_cast<QAbstractScrollArea*>(widget)) {
            registerArea(area);
        }
    }
}

void OverlayScrollBarManager::registerArea(QAbstractScrollArea* area)
{
    if (!area || states.contains(area)) {
        return;
    }

    auto* state = new State;
    state->area = area;
    state->sourceVertical = area->verticalScrollBar();
    state->sourceHorizontal = area->horizontalScrollBar();
    state->verticalEnabled = area->verticalScrollBarPolicy() != Qt::ScrollBarAlwaysOff;
    state->horizontalEnabled = area->horizontalScrollBarPolicy() != Qt::ScrollBarAlwaysOff;

    states.insert(area, state);

    const auto setupOverlay = [](QScrollBar* bar,
                                 QGraphicsOpacityEffect*& effect,
                                 QPropertyAnimation*& animation) {
        if (!bar) {
            return;
        }
        effect = new QGraphicsOpacityEffect(bar);
        effect->setOpacity(0.0);
        bar->setGraphicsEffect(effect);

        animation = new QPropertyAnimation(effect, QByteArrayLiteral("opacity"), bar);
        animation->setEasingCurve(QEasingCurve::OutCubic);
    };

    if (state->verticalEnabled) {
        state->overlayVertical = createOverlay(*area, Qt::Vertical, VerticalObjectName);
        setupOverlay(state->overlayVertical, state->verticalOpacity, state->verticalAnimation);
        area->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    }
    if (state->horizontalEnabled) {
        state->overlayHorizontal = createOverlay(*area, Qt::Horizontal, HorizontalObjectName);
        setupOverlay(state->overlayHorizontal, state->horizontalOpacity, state->horizontalAnimation);
        area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    }

    state->fadeTimer = new QTimer(area);
    state->fadeTimer->setSingleShot(true);
    state->fadeTimer->setInterval(FadeDelayMs);

    const auto connectSource = [this, state](QScrollBar* source, QScrollBar* overlay) {
        if (!source || !overlay) {
            return;
        }
        connect(source, &QScrollBar::rangeChanged, this, [this, state](int, int) {
            sync(*state);
            layout(*state);
            if (cursorOverOverlay(*state)) {
                if (state->fadeTimer) {
                    state->fadeTimer->stop();
                }
                reveal(*state);
            }
        });
        connect(source, &QScrollBar::valueChanged, this, [this, state](int) {
            sync(*state);
            reveal(*state);
            if (cursorOverOverlay(*state)) {
                if (state->fadeTimer) {
                    state->fadeTimer->stop();
                }
            } else {
                scheduleFade(*state);
            }
        });
        connect(overlay, &QScrollBar::valueChanged, this, [source](int value) {
            source->setValue(value);
        });
        connect(overlay, &QScrollBar::sliderPressed, this, [this, state] {
            if (state->fadeTimer) {
                state->fadeTimer->stop();
            }
            reveal(*state);
        });
        connect(overlay, &QScrollBar::sliderReleased, this, [this, state] {
            if (cursorOverOverlay(*state)) {
                reveal(*state);
            } else {
                scheduleFade(*state);
            }
        });
    };

    connectSource(state->sourceVertical, state->overlayVertical);
    connectSource(state->sourceHorizontal, state->overlayHorizontal);

    connect(state->fadeTimer, &QTimer::timeout, area, [this, state] {
        const bool dragging = (state->overlayVertical && state->overlayVertical->isSliderDown())
            || (state->overlayHorizontal && state->overlayHorizontal->isSliderDown());
        if (dragging || cursorOverOverlay(*state)) {
            reveal(*state);
            return;
        }
        fade(*state);
    });

    connect(area, &QObject::destroyed, this, [this, area] {
        delete states.take(area);
    });

    sync(*state);
    updatePalette(*state);
    layout(*state);
    state->cursorInside = containsCursor(*state);
    if (state->cursorInside) {
        reveal(*state);
        if (!cursorOverOverlay(*state)) {
            scheduleFade(*state);
        }
    }
}

OverlayScrollBarManager::State* OverlayScrollBarManager::stateForWidget(QWidget* widget) const
{
    for (QWidget* current = widget; current; current = current->parentWidget()) {
        if (auto* area = qobject_cast<QAbstractScrollArea*>(current)) {
            const auto it = states.constFind(area);
            if (it != states.cend()) {
                return it.value();
            }
        }
    }
    return nullptr;
}

void OverlayScrollBarManager::sync(State& state)
{
    syncBar(state.sourceVertical, state.overlayVertical);
    syncBar(state.sourceHorizontal, state.overlayHorizontal);

    if (state.overlayVertical) {
        if (state.verticalEnabled && scrollable(state.sourceVertical)) {
            state.overlayVertical->show();
        } else {
            state.overlayVertical->hide();
        }
    }
    if (state.overlayHorizontal) {
        if (state.horizontalEnabled && scrollable(state.sourceHorizontal)) {
            state.overlayHorizontal->show();
        } else {
            state.overlayHorizontal->hide();
        }
    }
}

void OverlayScrollBarManager::layout(State& state)
{
    if (!state.area || !state.area->viewport()) {
        return;
    }

    const QRect viewportRect = state.area->viewport()->geometry();
    const bool verticalScrollable = state.verticalEnabled && scrollable(state.sourceVertical);
    const bool horizontalScrollable = state.horizontalEnabled && scrollable(state.sourceHorizontal);

    if (state.overlayVertical) {
        const int bottomCut = horizontalScrollable ? ScrollBarHitThickness : 0;
        const int height = std::max(0, viewportRect.height() - 2 * ScrollBarEndInset - bottomCut);
        const int x = viewportRect.right() - ScrollBarHitThickness + 1;
        state.overlayVertical->setGeometry(x,
                                           viewportRect.top() + ScrollBarEndInset,
                                           ScrollBarHitThickness,
                                           height);
        state.overlayVertical->raise();
        if (!verticalScrollable) {
            state.overlayVertical->hide();
        }
    }

    if (state.overlayHorizontal) {
        const int rightCut = verticalScrollable ? ScrollBarHitThickness : 0;
        const int width = std::max(0, viewportRect.width() - 2 * ScrollBarEndInset - rightCut);
        const int y = viewportRect.bottom() - ScrollBarHitThickness + 1;
        state.overlayHorizontal->setGeometry(viewportRect.left() + ScrollBarEndInset,
                                             y,
                                             width,
                                             ScrollBarHitThickness);
        state.overlayHorizontal->raise();
        if (!horizontalScrollable) {
            state.overlayHorizontal->hide();
        }
    }
}

void OverlayScrollBarManager::updatePalette(State& state)
{
    if (!state.area || !state.area->viewport()) {
        return;
    }

    QWidget* viewport = state.area->viewport();
    const QPalette viewportPalette = viewport->palette();
    QColor background = viewportPalette.color(viewport->backgroundRole());
    if (!background.isValid()) {
        background = viewportPalette.color(QPalette::Base);
    }

    // Keep the handle neutral and let opacity, rather than hue or geometry,
    // communicate hover/idle state. This avoids accent-colored hover under
    // Breeze and keeps the shape identical throughout the animation.
    const bool darkBackground = background.lightnessF() < 0.5;
    QColor handle = darkBackground ? QColor(Qt::white) : QColor(Qt::black);
    handle.setAlpha(darkBackground ? 185 : 160);

    const QString styleSheet = overlayStyleSheet(handle);
    if (state.overlayVertical) {
        state.overlayVertical->setStyleSheet(styleSheet);
    }
    if (state.overlayHorizontal) {
        state.overlayHorizontal->setStyleSheet(styleSheet);
    }
}

void OverlayScrollBarManager::reveal(State& state)
{
    sync(state);
    layout(state);

    if (state.overlayVertical && state.verticalEnabled && scrollable(state.sourceVertical)) {
        state.overlayVertical->show();
        state.overlayVertical->raise();
        animateOpacity(state.verticalOpacity,
                       state.verticalAnimation,
                       1.0,
                       RevealDurationMs);
    }
    if (state.overlayHorizontal && state.horizontalEnabled && scrollable(state.sourceHorizontal)) {
        state.overlayHorizontal->show();
        state.overlayHorizontal->raise();
        animateOpacity(state.horizontalOpacity,
                       state.horizontalAnimation,
                       1.0,
                       RevealDurationMs);
    }
}

void OverlayScrollBarManager::fade(State& state)
{
    const bool dragging = (state.overlayVertical && state.overlayVertical->isSliderDown())
        || (state.overlayHorizontal && state.overlayHorizontal->isSliderDown());
    if (dragging || cursorOverOverlay(state)) {
        reveal(state);
        return;
    }

    animateOpacity(state.verticalOpacity,
                   state.verticalAnimation,
                   0.0,
                   FadeDurationMs);
    animateOpacity(state.horizontalOpacity,
                   state.horizontalAnimation,
                   0.0,
                   FadeDurationMs);
}

void OverlayScrollBarManager::scheduleFade(State& state)
{
    if (state.fadeTimer) {
        state.fadeTimer->start();
    }
}

bool OverlayScrollBarManager::containsCursor(const State& state) const
{
    if (!state.area || !state.area->isVisible()) {
        return false;
    }
    const QPoint local = state.area->mapFromGlobal(QCursor::pos());
    return state.area->rect().contains(local);
}

bool OverlayScrollBarManager::cursorOverOverlay(const State& state) const
{
    if (!state.area || !state.area->isVisible()) {
        return false;
    }

    const QPoint local = state.area->mapFromGlobal(QCursor::pos());
    const auto contains = [local](const QScrollBar* bar) {
        return bar && scrollable(bar) && bar->geometry().contains(local);
    };
    return contains(state.overlayVertical) || contains(state.overlayHorizontal);
}

bool OverlayScrollBarManager::eventFilter(QObject* watched, QEvent* event)
{
    if (!watched || !event) {
        return QObject::eventFilter(watched, event);
    }

    if (auto* area = qobject_cast<QAbstractScrollArea*>(watched)) {
        if ((event->type() == QEvent::Polish || event->type() == QEvent::Show)
            && !states.contains(area)) {
            registerArea(area);
        }

        const auto it = states.constFind(area);
        if (it != states.cend()) {
            State& state = *it.value();
            switch (event->type()) {
            case QEvent::Resize:
            case QEvent::LayoutRequest:
            case QEvent::Show:
                QTimer::singleShot(0, area, [this, statePtr = it.value()] {
                    sync(*statePtr);
                    layout(*statePtr);
                    statePtr->cursorInside = containsCursor(*statePtr);
                    if (statePtr->cursorInside) {
                        reveal(*statePtr);
                        if (cursorOverOverlay(*statePtr)) {
                            if (statePtr->fadeTimer) {
                                statePtr->fadeTimer->stop();
                            }
                        } else {
                            scheduleFade(*statePtr);
                        }
                    }
                });
                break;
            case QEvent::PaletteChange:
            case QEvent::ApplicationPaletteChange:
            case QEvent::StyleChange:
                updatePalette(state);
                layout(state);
                break;
            case QEvent::Hide:
                state.cursorInside = false;
                if (state.fadeTimer) {
                    state.fadeTimer->stop();
                }
                if (state.verticalAnimation) {
                    state.verticalAnimation->stop();
                }
                if (state.horizontalAnimation) {
                    state.horizontalAnimation->stop();
                }
                if (state.verticalOpacity) {
                    state.verticalOpacity->setOpacity(0.0);
                }
                if (state.horizontalOpacity) {
                    state.horizontalOpacity->setOpacity(0.0);
                }
                break;
            default:
                break;
            }
        }
    }

    QWidget* widget = qobject_cast<QWidget*>(watched);
    State* state = widget ? stateForWidget(widget) : nullptr;
    if (state) {
        const bool overlayWidget = widget == state->overlayVertical
            || widget == state->overlayHorizontal;
        const bool areaBoundary = widget == state->area
            || (state->area && widget == state->area->viewport());

        switch (event->type()) {
        case QEvent::Enter:
            if (overlayWidget) {
                if (state->fadeTimer) {
                    state->fadeTimer->stop();
                }
                reveal(*state);
            } else if (areaBoundary && !state->cursorInside && containsCursor(*state)) {
                state->cursorInside = true;
                reveal(*state);
                if (cursorOverOverlay(*state)) {
                    if (state->fadeTimer) {
                        state->fadeTimer->stop();
                    }
                } else {
                    scheduleFade(*state);
                }
            }
            break;
        case QEvent::Leave:
            if (overlayWidget && state->area) {
                QTimer::singleShot(0, state->area, [this, state] {
                    if (cursorOverOverlay(*state)) {
                        if (state->fadeTimer) {
                            state->fadeTimer->stop();
                        }
                        reveal(*state);
                    } else if (containsCursor(*state)) {
                        state->cursorInside = true;
                        scheduleFade(*state);
                    } else {
                        state->cursorInside = false;
                        fade(*state);
                    }
                });
            } else if (areaBoundary && state->area) {
                QTimer::singleShot(0, state->area, [this, state] {
                    if (!containsCursor(*state)) {
                        state->cursorInside = false;
                        fade(*state);
                    }
                });
            }
            break;
        case QEvent::Wheel:
            state->cursorInside = containsCursor(*state);
            reveal(*state);
            if (cursorOverOverlay(*state)) {
                if (state->fadeTimer) {
                    state->fadeTimer->stop();
                }
            } else {
                scheduleFade(*state);
            }
            break;
        default:
            break;
        }
    }

    return QObject::eventFilter(watched, event);
}

} // namespace Mattermost
