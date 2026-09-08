#pragma once

#include <algorithm>

#include <QList>
#include <QSplitter>

namespace Mattermost {

inline bool ensureThreadPaneExpanded(QSplitter& splitter, bool forceDefaultSize = false)
{
    const QList<int> sizes = splitter.sizes();
    if (!forceDefaultSize && sizes.size() >= 2 && sizes.at(1) > 0) {
        return false;
    }

    const int width = std::max(900, splitter.width());
    splitter.setSizes({std::max(480, width * 2 / 3),
                       std::max(320, width / 3)});
    return true;
}

} // namespace Mattermost
