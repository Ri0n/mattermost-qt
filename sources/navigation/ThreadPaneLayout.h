#pragma once

#include <algorithm>

#include <QList>
#include <QSplitter>

namespace Mattermost {

inline bool ensureThreadPaneExpanded(QSplitter& splitter, bool forceDefaultSize = false)
{
    // QSplitter::restoreState() persists orientation as well as sizes. The
    // thread surface is semantically a right-hand pane, so never let a stale
    // vertical splitter state move it below the channel after restoration.
    if (splitter.orientation() != Qt::Horizontal) {
        splitter.setOrientation(Qt::Horizontal);
    }

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
