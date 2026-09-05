#include "LongListWidget.h"

#include <algorithm>
#include <utility>

#include <QSignalBlocker>

namespace Mattermost {
namespace {

QBitArray removedBits(const QBitArray& source, int first, int count)
{
    const int oldSize = static_cast<int>(source.size());
    first = std::max(0, std::min(first, oldSize));
    count = std::max(0, std::min(count, oldSize - first));

    QBitArray result(oldSize - count, false);
    for (int index = 0; index < first; ++index) {
        result.setBit(index, source.testBit(index));
    }
    for (int index = first + count; index < oldSize; ++index) {
        result.setBit(index - count, source.testBit(index));
    }
    return result;
}

} // namespace

void LongListWidget::HeightIndex::remove(int first, int count)
{
    first = std::max(0, std::min(first, static_cast<int>(values.size())));
    count = std::max(0, std::min(count, static_cast<int>(values.size()) - first));
    for (int offset = 0; offset < count; ++offset) {
        values.removeAt(first);
    }
    rebuildFenwick();
}

void LongListWidget::removeItems(int first, int count)
{
    first = std::max(0, std::min(first, logicalCount));
    count = std::max(0, std::min(count, logicalCount - first));
    if (count == 0) {
        return;
    }

    const int removedLast = first + count - 1;
    const int newCount = logicalCount - count;

    ViewAnchor anchor = captureAnchor();
    if (anchor.kind == ViewAnchor::Item) {
        if (anchor.index > removedLast) {
            anchor.index -= count;
        } else if (anchor.index >= first) {
            if (newCount > 0) {
                anchor.index = std::min(first, newCount - 1);
                anchor.offsetInsideItem = 0;
            } else {
                anchor = ViewAnchor();
            }
        }
    }

    if (viewportLock.index > removedLast) {
        viewportLock.index -= count;
    } else if (viewportLock.index >= first) {
        releaseViewportLock(true);
    }

    if (seekTarget > removedLast) {
        seekTarget -= count;
        ++seekGeneration;
    } else if (seekTarget >= first) {
        clearSeek();
    }

    const qint64 oldOffset = contentOffset();

    QHash<int, QWidget*> shiftedMaterialized;
    shiftedMaterialized.reserve(materialized.size());
    for (auto it = materialized.cbegin(); it != materialized.cend(); ++it) {
        const int index = it.key();
        QWidget* widget = it.value();
        if (index >= first && index <= removedLast) {
            if (widget) {
                widgetIndexes.remove(widget);
                widget->removeEventFilter(this);
                destroyItemWidget(index, widget);
            }
            continue;
        }

        const int shiftedIndex = index > removedLast ? index - count : index;
        shiftedMaterialized.insert(shiftedIndex, widget);
        if (widget) {
            widgetIndexes[widget] = shiftedIndex;
        }
    }
    materialized = std::move(shiftedMaterialized);

    QSet<int> shiftedDirty;
    for (int index : std::as_const(dirtyGeometry)) {
        if (index >= first && index <= removedLast) {
            continue;
        }
        shiftedDirty.insert(index > removedLast ? index - count : index);
    }
    dirtyGeometry = std::move(shiftedDirty);

    logicalCount = newCount;
    heights.remove(first, count);
    measured = removedBits(measured, first, count);
    available = removedBits(available, first, count);

    // Any outstanding range was expressed in the old coordinate system. Late
    // HTTP results can still populate the source by identity, but view-side
    // suppression must be rebuilt against the shifted logical indices.
    pendingRequest = QBitArray(logicalCount, false);

    QSignalBlocker blocker(verticalScrollBar());
    viewport()->setUpdatesEnabled(false);
    committingGeometry = true;
    updateScrollBarRange(oldOffset);
    if (hasViewportLock()) {
        restoreViewportLock();
    } else {
        restoreAnchor(anchor);
    }
    layoutMaterialized();
    committingGeometry = false;
    viewport()->setUpdatesEnabled(true);
    viewport()->update();

    lastVisibleRange = {};
    lastMaterializedRange = {};
    scheduleSync(seekActive ? RequestReason::Seek : RequestReason::Scroll);
    emitRangeChanges();
}

} // namespace Mattermost
