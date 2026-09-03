#pragma once

#include <algorithm>

#include <QColor>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QString>

namespace Mattermost::AvatarUtils {

inline QColor statusColor(const QString& status)
{
    if (status == QStringLiteral("online")) {
        return QColor(QStringLiteral("#3DB887"));
    }
    if (status == QStringLiteral("away")) {
        return QColor(QStringLiteral("#FFBC1F"));
    }
    if (status == QStringLiteral("dnd")) {
        return QColor(QStringLiteral("#D24B4E"));
    }
    return QColor(QStringLiteral("#8E8E8E"));
}

inline QPixmap circular(const QPixmap& source, int size)
{
    if (source.isNull() || size <= 0) {
        return {};
    }

    QPixmap result(size, size);
    result.fill(Qt::transparent);

    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    QPainterPath clip;
    clip.addEllipse(QRectF(0, 0, size, size));
    painter.setClipPath(clip);

    const QPixmap scaled = source.scaled(size, size,
                                         Qt::KeepAspectRatioByExpanding,
                                         Qt::SmoothTransformation);
    const int x = (size - scaled.width()) / 2;
    const int y = (size - scaled.height()) / 2;
    painter.drawPixmap(x, y, scaled);
    return result;
}

inline QPixmap withStatus(const QPixmap& source,
                          int size,
                          const QString& status,
                          int badgeSize = 12)
{
    QPixmap result = circular(source, size);
    if (result.isNull() || status.isEmpty()) {
        return result;
    }

    badgeSize = std::max(4, std::min(badgeSize, size));
    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF badge(size - badgeSize - 1,
                       size - badgeSize - 1,
                       badgeSize,
                       badgeSize);
    QPen outline(Qt::white);
    outline.setWidthF(std::max(1.5, badgeSize / 5.0));
    painter.setPen(outline);
    painter.setBrush(statusColor(status));
    painter.drawEllipse(badge.adjusted(1.0, 1.0, -1.0, -1.0));
    return result;
}

} // namespace Mattermost::AvatarUtils
