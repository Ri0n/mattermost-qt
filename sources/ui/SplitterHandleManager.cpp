/*
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#include "SplitterHandleManager.h"

#include <QApplication>
#include <QEvent>
#include <QPainter>
#include <QPalette>
#include <QSplitter>

namespace Mattermost {
namespace {

constexpr char InstalledProperty[] = "mattermostSplitterHandleManagerInstalled";

} // namespace

void SplitterHandleManager::install(QApplication& application)
{
    if (application.property(InstalledProperty).toBool()) {
        return;
    }
    application.setProperty(InstalledProperty, true);
    new SplitterHandleManager(application);
}

SplitterHandleManager::SplitterHandleManager(QApplication& application)
    : QObject(&application)
{
    application.installEventFilter(this);
}

bool SplitterHandleManager::eventFilter(QObject* watched, QEvent* event)
{
    auto* handle = qobject_cast<QSplitterHandle*>(watched);
    if (!handle || !event || event->type() != QEvent::Paint) {
        return QObject::eventFilter(watched, event);
    }

    const QWidget* panel = handle->parentWidget();
    const QColor background = panel
        ? panel->palette().color(QPalette::Window)
        : handle->palette().color(QPalette::Window);

    QPainter painter(handle);
    painter.fillRect(handle->rect(), background);
    return true;
}

} // namespace Mattermost
