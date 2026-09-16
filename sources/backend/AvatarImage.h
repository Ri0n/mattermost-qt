#pragma once

#include <QByteArray>
#include <QPixmap>

namespace Mattermost {

inline QPixmap decodeAvatarImage(const QByteArray& data)
{
    QPixmap pixmap;
    pixmap.loadFromData(data);
    return pixmap;
}

} // namespace Mattermost
