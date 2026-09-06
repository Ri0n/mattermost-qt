/**
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
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Mattermost-QT. if not, see https://www.gnu.org/licenses/.
 */

#pragma once

#include <QColor>
#include <QLabel>
#include <QPixmap>
#include <QString>

class QEvent;
class QPaintEvent;
class QResizeEvent;

namespace Mattermost {

/**
 * QLabel-compatible avatar that renders the same circular presence treatment
 * used in PostWidget. setPixmap() deliberately hides QLabel::setPixmap(): uic
 * and existing callers keep their old API while the widget owns presentation.
 */
class PresenceAvatarLabel final : public QLabel
{
    Q_OBJECT

public:
    explicit PresenceAvatarLabel(QWidget* parent = nullptr);

    void setPixmap(const QPixmap& pixmap);
    void setStatus(const QString& status);

    static bool isPresenceStatus(const QString& text);

protected:
    void changeEvent(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void refreshPixmap();

    QPixmap sourcePixmap;
    QString presenceStatus;
    QColor renderedBackground;
};

/**
 * Compatibility label for the sidebar header. Existing MainWindow code writes
 * online/away/dnd/offline as text; consume that value as avatar state instead
 * of displaying a second textual presence indicator.
 */
class PresenceStatusLabel final : public QLabel
{
    Q_OBJECT

public:
    explicit PresenceStatusLabel(QWidget* parent = nullptr);

    void setText(const QString& text);
};

} // namespace Mattermost
