/*
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "OverlayScrollBarManager.h"

#include <algorithm>

#include <QAbstractScrollArea>
#include <QApplication>
#include <QCursor>
#include <QEvent>
#include <QPalette>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTimer>
#include <QWidget>

namespace Mattermost {
namespace {

constexpr int ScrollBarThickness = 10;
constexpr int ScrollBarInset = 2;
constexpr int HideDelayMs = 650;
constexpr char InstalledProperty[] = "mattermostOverlayScrollBarsInstalled";
constexpr char VerticalObjectName[] = "mattermostOverlayVerticalScrollBar";
constexpr char HorizontalObjectName[] = "mattermostOverlayHorizontalScrollBar";

QString overlayStyleSheet()
{
    return QStringLiteral(
        "QScrollBar:vertical {"
        " background: transparent; border: 0; width: 10px; margin: 0;"
        "}"
        "QScrollBar::handle:vertical {"
        " background: palette(mid); border: 0; border-radius: 4px;"
        " min-height: 28px; margin: 1px 2px;"
        "}"
        "QScrollBar::handle:vertical:hover, QScrollBar::handle:vertical:pressed {"
        " background: palette(highlight); margin: 1px;"
        "}"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {"
        " height: 0; border: 0; background: transparent;"
        "}"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {"
        " background: transparent;"
        "}"
        "QScrollBar:horizontal {"
        " background: transparent; border: 0; height: 10px; margin: 0;"
        "}"
        "QScrollBar::handle:horizontal {"
        " background: palette(mid); border: 0; border-radius: 4px;"
        " min-width: 28px; margin: 2px 1px;"
        "}"
        "QScrollBar::handle:horizontal:hover, QScrollBar::handle:horizontal:pressed {"
        " background: palette(highlight); margin: 1px;"
        "}"
        "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {"
        " width: 0; border: 0; background: transparent;"
        "}"
        "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {"
        " background: transparent;"
        "}");
}

QScrollBar* createOverlay(QAbstractScrollArea& area,
                          Qt::Orientation orientation,
                          const char* objectName)
{
    auto* bar = new QScrollBar(orientation, &area);
    bar->setObjectName(QString::fromLatin1(objectName));
    bar->setFocusPolicy(Qt::NoFocus);
    bar->setMouseTracking(true);
    bar->setStyleSheet(overlayStyleSheet());
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

} // namespace

struct OverlayScrollBarManager::State {
    QAbstractScrollArea* area = nullptr;
    QScrollBar* sourceVertical = nullptr;
    QScrollBar* sourceHorizontal = nullptr;
    QScrollBar* overlayVertical = nullptr;
    QScrollBar* overlayHorizontal = nullptr;
    QTimer* hideTimer = nullptr;
    bool verticalEnabled = false;
    bool horizontalEnabled = false;
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

    if (state->verticalEnabled) {
        state->overlayVertical = createOverlay(*area, Qt::Vertical, VerticalObjectName);
        area->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    }
    if (state->horizontalEnabled) {
        state->overlayHorizontal = createOverlay(*area, Qt::Horizontal, HorizontalObjectName);
        area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    }

    state->hideTimer = new QTimer(area);
    state->hideTimer->setSingleShot(true);
    state->hideTimer->setInterval(HideDelayMs);

    const auto connectSource = [this, state](QScrollBar* source, QScrollBar* overlay) {
        if (!source || !overlay) {
            return;
        }
        connect(source, &QScrollBar::rangeChanged, this, [this, state](int, int) {
            sync(*state);
            layout(*state);
            if (containsCursor(*state)) {
                show(*state);
            }
        });
        connect(source, &QScrollBar::valueChanged, this, [this, state](int) {
            sync(*state);
            show(*state);
            scheduleHide(*state);
        });
        connect(overlay, &QScrollBar::valueChanged, this, [source](int value) {
            source->setValue(value);
        });
        connect(overlay, &QScrollBar::sliderPressed, this, [this, state] {
            if (state->hideTimer) {
                state->hideTimer->stop();
            }
            show(*state);
        });
        connect(overlay, &QScrollBar::sliderReleased, this, [this, state] {
            scheduleHide(*state);
        });
    };

    connectSource(state->sourceVertical, state->overlayVertical);
    connectSource(state->sourceHorizontal, state->overlayHorizontal);

    connect(state->hideTimer, &QTimer::timeout, area, [this, state] {
        const bool dragging = (state->overlayVertical && state->overlayVertical->isSliderDown())
            || (state->overlayHorizontal && state->overlayHorizontal->isSliderDown());
        if (dragging || containsCursor(*state)) {
            return;
        }
        hide(*state);
    });

    connect(area, &QObject::destroyed, this, [this, area] {
        delete states.take(area);
    });

    sync(*state);
    updatePalette(*state);
    layout(*state);
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

    if (state.overlayVertical && !scrollable(state.sourceVertical)) {
        state.overlayVertical->hide();
    }
    if (state.overlayHorizontal && !scrollable(state.sourceHorizontal)) {
        state.overlayHorizontal->hide();
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
        const int bottomCut = horizontalScrollable ? ScrollBarThickness + ScrollBarInset : 0;
        const int height = std::max(0, viewportRect.height() - 2 * ScrollBarInset - bottomCut);
        const int x = viewportRect.right() - ScrollBarThickness + 1 - ScrollBarInset;
        state.overlayVertical->setGeometry(x,
                                           viewportRect.top() + ScrollBarInset,
                                           ScrollBarThickness,
                                           height);
        state.overlayVertical->raise();
        if (!verticalScrollable) {
            state.overlayVertical->hide();
        }
    }

    if (state.overlayHorizontal) {
        const int rightCut = verticalScrollable ? ScrollBarThickness + ScrollBarInset : 0;
        const int width = std::max(0, viewportRect.width() - 2 * ScrollBarInset - rightCut);
        const int y = viewportRect.bottom() - ScrollBarThickness + 1 - ScrollBarInset;
        state.overlayHorizontal->setGeometry(viewportRect.left() + ScrollBarInset,
                                             y,
                                             width,
                                             ScrollBarThickness);
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

    // Scrollbars are deliberately neutral rather than using the application's
    // accent/highlight color. Pick their polarity from the actual viewport
    // background so a dark desktop theme gets a light handle and vice versa.
    const bool darkBackground = background.lightnessF() < 0.5;
    QColor normal = darkBackground ? QColor(Qt::white) : QColor(Qt::black);
    QColor hover = normal;
    normal.setAlpha(darkBackground ? 105 : 85);
    hover.setAlpha(darkBackground ? 160 : 145);

    const auto updateBar = [normal, hover](QScrollBar* bar) {
        if (!bar) {
            return;
        }
        QPalette palette = bar->palette();
        palette.setColor(QPalette::Mid, normal);
        palette.setColor(QPalette::Highlight, hover);
        bar->setPalette(palette);
    };
    updateBar(state.overlayVertical);
    updateBar(state.overlayHorizontal);
}

void OverlayScrollBarManager::show(State& state)
{
    sync(state);
    layout(state);

    if (state.overlayVertical && state.verticalEnabled && scrollable(state.sourceVertical)) {
        state.overlayVertical->show();
        state.overlayVertical->raise();
    }
    if (state.overlayHorizontal && state.horizontalEnabled && scrollable(state.sourceHorizontal)) {
        state.overlayHorizontal->show();
        state.overlayHorizontal->raise();
    }
}

void OverlayScrollBarManager::hide(State& state)
{
    if (state.overlayVertical && !state.overlayVertical->isSliderDown()) {
        state.overlayVertical->hide();
    }
    if (state.overlayHorizontal && !state.overlayHorizontal->isSliderDown()) {
        state.overlayHorizontal->hide();
    }
}

void OverlayScrollBarManager::scheduleHide(State& state)
{
    if (state.hideTimer) {
        state.hideTimer->start();
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
                    if (containsCursor(*statePtr)) {
                        show(*statePtr);
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
                hide(state);
                break;
            default:
                break;
            }
        }
    }

    QWidget* widget = qobject_cast<QWidget*>(watched);
    State* state = widget ? stateForWidget(widget) : nullptr;
    if (state) {
        switch (event->type()) {
        case QEvent::Enter:
            if (state->hideTimer) {
                state->hideTimer->stop();
            }
            show(*state);
            break;
        case QEvent::Leave:
            scheduleHide(*state);
            break;
        case QEvent::Wheel:
            show(*state);
            scheduleHide(*state);
            break;
        default:
            break;
        }
    }

    return QObject::eventFilter(watched, event);
}

} // namespace Mattermost
