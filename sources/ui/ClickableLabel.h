/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#pragma once

#include <QLabel>

class QMouseEvent;

namespace Mattermost {

/** QLabel that exposes a semantic left-click without owning the click action. */
class ClickableLabel : public QLabel
{
    Q_OBJECT

public:
    explicit ClickableLabel(QWidget* parent = nullptr);

signals:
    void clicked();

protected:
    void mouseReleaseEvent(QMouseEvent* event) override;
};

} // namespace Mattermost
