/*
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#pragma once

#include <QObject>

class QApplication;
class QEvent;

namespace Mattermost {

/** Paint splitter handles with the surrounding panel background. */
class SplitterHandleManager final : public QObject
{
public:
    static void install(QApplication& application);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    explicit SplitterHandleManager(QApplication& application);
};

} // namespace Mattermost
