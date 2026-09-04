#include "LongListWidget.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <utility>

#include <QEvent>
#include <QLayout>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QWheelEvent>

namespace Mattermost {

namespace {

constexpr int MinimumPrefetchItems = 5;

bool sameRange(const LongListWidget::Range& lhs, const LongListWidget::Range& rhs)
{
    return lhs.first == rhs.first && lhs.last == rhs.last;
}

int boundedHeight(int height)
{
    return std::max(1, height);
}

QBitArray insertedBits(const QBitArray& source, int first, int count)
{
    const int oldSize = static_cast<int>(source.size());
    first = std::max(0, std::min(first, oldSize));
    count = std::max(0, count);

    QBitArray result(oldSize + count, false);
    for (int index = 0; index < first; ++index) {
        result.setBit(index, source.testBit(index));
    }
    for (int index = first; index < oldSize; ++index) {
        result.setBit(index + count, source.testBit(index));
    }
    return result;
}

} // namespace

void LongListWidget::HeightIndex::reset(int count, int height)
{
    count = std::max(0, count);
    height = boundedHeight(height);
    values.fill(height, count);
    rebuildFenwick();
}

void LongListWidget::HeightIndex::resize(int count, int height)
{
    count = std::max(0, count);
    const QVector<int> previous = values;
    reset(count, height);
    const int preserved = std::min(static_cast<int>(previous.size()), count);
    for (int index = 0; index < preserved; ++index) {
        setValue(index, previous.at(index));
    }
}

void LongListWidget::HeightIndex::insert(int first, int count, int height)
{
    first = std::max(0, std::min(first, static_cast<int>(values.size())));
    count = std::max(0, count);
    height = boundedHeight(height);
    for (int offset = 0; offset < count; ++offset) {
        values.insert(first + offset, height);
    }
    rebuildFenwick();
}

int LongListWidget::HeightIndex::value(int index) const
{
    return index >= 0 && index < values.size() ? values.at(index) : 0;
}

void LongListWidget::HeightIndex::setValue(int index, int value)
{
    if (index < 0 || index >= values.size()) {
        return;
    }
    value = boundedHeight(value);
    const qint64 delta = static_cast<qint64>(value) - values.at(index);
    if (delta == 0) {
        return;
    }
    values[index] = value;
    add(index, delta);
}

void LongListWidget::HeightIndex::add(int index, qint64 delta)
{
    for (int pos = index + 1; pos < fenwick.size(); pos += pos & -pos) {
        fenwick[pos] += delta;
    }
}

void LongListWidget::HeightIndex::rebuildFenwick()
{
    fenwick.fill(0, values.size() + 1);
    for (int index = 0; index < values.size(); ++index) {
        add(index, values.at(index));
    }
}

qint64 LongListWidget::HeightIndex::prefixHeight(int count) const
{
    count = std::max(0, std::min(count, static_cast<int>(values.size())));
    qint64 result = 0;
    for (int pos = count; pos > 0; pos -= pos & -pos) {
        result += fenwick.at(pos);
    }
    return result;
}

int LongListWidget::HeightIndex::indexAtPixel(qint64 pixel) const
{
    if (values.isEmpty()) {
        return -1;
    }

    const qint64 total = totalHeight();
    if (total <= 0) {
        return 0;
    }
    pixel = std::max<qint64>(0, std::min(pixel, total - 1));

    int index = 0;
    qint64 accumulated = 0;
    int bit = 1;
    while ((bit << 1) < fenwick.size()) {
        bit <<= 1;
    }

    for (; bit != 0; bit >>= 1) {
        const int next = index + bit;
        if (next < fenwick.size() && accumulated + fenwick.at(next) <= pixel) {
            index = next;
            accumulated += fenwick.at(next);
        }
    }

    return std::min(index, static_cast<int>(values.size()) - 1);
}

LongListWidget::LongListWidget(QWidget* parent)
    : QAbstractScrollArea(parent)
{
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setFocusPolicy(Qt::StrongFocus);
    viewport()->setAutoFillBackground(true);

    syncTimer.setSingleShot(true);
    connect(&syncTimer, &QTimer::timeout, this, &LongListWidget::synchronize);

    geometryTimer.setSingleShot(true);
    connect(&geometryTimer, &QTimer::timeout, this, &LongListWidget::commitGeometry);

    seekTimer.setSingleShot(true);
    seekTimer.setInterval(seekDebounceInterval);
    connect(&seekTimer, &QTimer::timeout, this, &LongListWidget::activateSeekTarget);

    viewportLockTimer.setSingleShot(true);
    connect(&viewportLockTimer, &QTimer::timeout, this, [this] {
        releaseViewportLock(true);
    });

    QScrollBar* bar = verticalScrollBar();
    connect(bar, &QScrollBar::valueChanged, this, [this] { onScrollValueChanged(); });
    connect(bar, &QScrollBar::sliderMoved, this, &LongListWidget::onSliderMoved);
    connect(bar, &QScrollBar::actionTriggered, this, [this](int) {
        // actionTriggered is a user scrollbar action. setValue() used by our
        // geometry transactions does not emit it. Defer until Qt has applied
        // the action's new value so observers see the final at-end state.
        QTimer::singleShot(0, this, [this] { noteUserViewportChange(); });
    });
    connect(bar, &QScrollBar::sliderReleased, this, [this, bar] {
        onSliderMoved(bar->value());
        seekTimer.stop();
        activateSeekTarget();
        noteUserViewportChange();
    });
}

LongListWidget::~LongListWidget()
{
    const auto widgets = materialized;
    materialized.clear();
    widgetIndexes.clear();
    for (auto it = widgets.cbegin(); it != widgets.cend(); ++it) {
        destroyItemWidget(it.key(), it.value());
    }
}

void LongListWidget::setItemCount(int count)
{
    count = std::max(0, count);
    if (count == logicalCount) {
        return;
    }

    // Growing the logical tail changes maximumContentOffset immediately, before
    // commitGeometry() can capture its anchor. Remember an existing sticky-end
    // state while the old geometry is still authoritative and restore it after
    // resizing the height index. A semantic viewport lock has stronger intent
    // than sticky-bottom and is restored by commitGeometry() instead.
    const bool preserveBottom = logicalCount > 0 && !hasViewportLock() && isAtEnd();

    const int oldCount = logicalCount;
    logicalCount = count;
    heights.resize(count, defaultHeight);
    measured.resize(count);
    available.resize(count);
    pendingRequest.resize(count);

    for (int index = oldCount; index < count; ++index) {
        heights.setValue(index, estimatedItemHeight(index));
    }

    const QVector<int> current = materializedIndices();
    for (int index : current) {
        if (index >= count) {
            QWidget* widget = materialized.take(index);
            if (widget) {
                widgetIndexes.remove(widget);
                widget->removeEventFilter(this);
                destroyItemWidget(index, widget);
            }
        }
    }

    if (seekTarget >= count) {
        clearSeek();
    }
    if (viewportLock.index >= count) {
        releaseViewportLock(true);
    }

    commitGeometry();
    if (preserveBottom && logicalCount > 0) {
        scrollToEnd();
    } else {
        scheduleSync(RequestReason::Initial);
    }
}

void LongListWidget::insertItems(int first, int count)
{
    first = std::max(0, std::min(first, logicalCount));
    count = std::max(0, count);
    if (count == 0) {
        return;
    }

    ViewAnchor anchor = captureAnchor();
    if (anchor.kind == ViewAnchor::Item && anchor.index >= first) {
        anchor.index += count;
    }
    if (viewportLock.index >= first) {
        viewportLock.index += count;
    }
    const qint64 oldOffset = contentOffset();

    logicalCount += count;
    heights.insert(first, count, defaultHeight);
    measured = insertedBits(measured, first, count);
    available = insertedBits(available, first, count);

    // A structural shift changes the coordinate system of any outstanding
    // logical-range request. Drop view-side suppression; the service layer still
    // coalesces equivalent HTTP requests, and late results remain valid cache/data
    // input without leaving permanently shifted pending bits behind.
    pendingRequest = QBitArray(logicalCount, false);

    for (int index = 0; index < logicalCount; ++index) {
        if (!measured.testBit(index)) {
            heights.setValue(index, estimatedItemHeight(index));
        }
    }

    QHash<int, QWidget*> shiftedMaterialized;
    shiftedMaterialized.reserve(materialized.size());
    for (auto it = materialized.cbegin(); it != materialized.cend(); ++it) {
        const int shiftedIndex = it.key() >= first ? it.key() + count : it.key();
        shiftedMaterialized.insert(shiftedIndex, it.value());
        widgetIndexes[it.value()] = shiftedIndex;
    }
    materialized = std::move(shiftedMaterialized);

    QSet<int> shiftedDirty;
    for (int index : std::as_const(dirtyGeometry)) {
        shiftedDirty.insert(index >= first ? index + count : index);
    }
    dirtyGeometry = std::move(shiftedDirty);

    if (seekTarget >= first) {
        seekTarget += count;
        ++seekGeneration;
    }

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

void LongListWidget::setDefaultItemHeight(int height)
{
    height = boundedHeight(height);
    if (defaultHeight == height) {
        return;
    }
    defaultHeight = height;

    for (int index = 0; index < logicalCount; ++index) {
        if (!measured.testBit(index)) {
            heights.setValue(index, estimatedItemHeight(index));
        }
    }
    commitGeometry();
    scheduleSync(RequestReason::Scroll);
}

void LongListWidget::setMaterializationLimit(int count)
{
    maxMaterializedItems = std::max(1, count);
    scheduleSync(RequestReason::Scroll);
}

void LongListWidget::setRequestBlockSize(int count)
{
    blockSize = std::max(1, count);
}

void LongListWidget::setPrefetchScreens(int screens)
{
    bufferScreens = std::max(0, screens);
    scheduleSync(RequestReason::Scroll);
}

void LongListWidget::setSeekDebounceMs(int milliseconds)
{
    seekDebounceInterval = std::max(0, milliseconds);
    seekTimer.setInterval(seekDebounceInterval);
}

void LongListWidget::setRangeAvailable(int first, int last, bool isAvailable)
{
    if (logicalCount <= 0) {
        return;
    }
    first = std::max(0, first);
    last = std::min(logicalCount - 1, last);
    if (last < first) {
        return;
    }

    // A measured height belongs to the concrete logical identity that produced
    // it. If a provisional identity leaves a slot, that measurement must not be
    // reused as if it described the unknown item that will eventually occupy the
    // slot. Capture viewport intent before resetting any such height.
    const ViewAnchor anchor = !isAvailable ? captureAnchor() : ViewAnchor();
    const qint64 oldOffset = contentOffset();
    bool geometryChanged = false;

    for (int index = first; index <= last; ++index) {
        available.setBit(index, isAvailable);
        pendingRequest.clearBit(index);
        if (!isAvailable) {
            QWidget* widget = materialized.take(index);
            if (widget) {
                widgetIndexes.remove(widget);
                widget->removeEventFilter(this);
                destroyItemWidget(index, widget);
            }
            dirtyGeometry.remove(index);
            if (measured.testBit(index)) {
                measured.clearBit(index);
                heights.setValue(index, estimatedItemHeight(index));
                geometryChanged = true;
            }
        }
    }

    if (geometryChanged) {
        QSignalBlocker blocker(verticalScrollBar());
        viewport()->setUpdatesEnabled(false);
        committingGeometry = true;
        updateScrollBarRange(oldOffset);
        if (seekActive && seekTarget >= 0 && materialized.contains(seekTarget)) {
            restoreSeekTarget();
        } else if (hasViewportLock()) {
            restoreViewportLock();
        } else {
            restoreAnchor(anchor);
        }
        layoutMaterialized();
        committingGeometry = false;
        viewport()->setUpdatesEnabled(true);
        viewport()->update();
        emitRangeChanges();
    }

    scheduleSync(seekActive ? RequestReason::Seek : RequestReason::Scroll);
}

bool LongListWidget::isItemAvailable(int index) const
{
    return index >= 0 && index < logicalCount && available.testBit(index);
}

void LongListWidget::itemsChanged(int first, int last)
{
    first = std::max(0, first);
    last = std::min(logicalCount - 1, last);
    for (int index = first; index <= last; ++index) {
        if (materialized.contains(index)) {
            dirtyGeometry.insert(index);
        }
    }
    scheduleGeometryCommit();
}

QWidget* LongListWidget::itemWidget(int index) const
{
    return materialized.value(index, nullptr);
}

QVector<int> LongListWidget::materializedIndices() const
{
    QVector<int> result;
    result.reserve(materialized.size());
    for (auto it = materialized.cbegin(); it != materialized.cend(); ++it) {
        result.push_back(it.key());
    }
    std::sort(result.begin(), result.end());
    return result;
}

LongListWidget::Range LongListWidget::visibleRange() const
{
    Range result;
    if (logicalCount <= 0 || viewport()->height() <= 0 || heights.totalHeight() <= 0) {
        return result;
    }

    const qint64 top = contentOffset();
    const qint64 bottom = std::min<qint64>(heights.totalHeight() - 1,
        top + std::max(0, viewport()->height() - 1));
    result.first = heights.indexAtPixel(top);
    result.last = heights.indexAtPixel(bottom);
    return result;
}

LongListWidget::Range LongListWidget::materializedRange() const
{
    Range result;
    const QVector<int> indices = materializedIndices();
    if (!indices.isEmpty()) {
        result.first = indices.first();
        result.last = indices.last();
    }
    return result;
}

qint64 LongListWidget::contentHeight() const
{
    return heights.totalHeight();
}

int LongListWidget::indexAtViewportPosition(int viewportY) const
{
    if (logicalCount <= 0) {
        return -1;
    }
    const qint64 pixel = contentOffset() + std::max(0, viewportY);
    return heights.indexAtPixel(pixel);
}

void LongListWidget::scrollToIndex(int index, Alignment alignment)
{
    if (index < 0 || index >= logicalCount) {
        return;
    }

    if (hasViewportLock() && viewportLock.index != index) {
        releaseViewportLock(true);
    }

    clearSeek();
    const qint64 itemTop = heights.prefixHeight(index);
    const qint64 itemBottom = heights.prefixHeight(index + 1);
    const qint64 currentTop = contentOffset();
    const qint64 currentBottom = currentTop + viewport()->height();
    qint64 target = currentTop;

    switch (alignment) {
    case Alignment::Top:
        target = itemTop;
        break;
    case Alignment::Center:
        target = itemTop - (viewport()->height() - heights.value(index)) / 2;
        break;
    case Alignment::Bottom:
        target = itemBottom - viewport()->height();
        break;
    case Alignment::EnsureVisible:
        if (itemTop < currentTop) {
            target = itemTop;
        } else if (itemBottom > currentBottom) {
            target = itemBottom - viewport()->height();
        }
        break;
    }

    internalScrollChange = true;
    verticalScrollBar()->setValue(scrollValueForContentOffset(target));
    internalScrollChange = false;
    layoutMaterialized();
    if (hasViewportLock() && viewportLock.index == index) {
        captureViewportLockFraction();
    }
    scheduleSync(RequestReason::EnsureVisible);
}

void LongListWidget::scrollToEnd()
{
    if (hasViewportLock()) {
        releaseViewportLock(true);
    }
    clearSeek();
    internalScrollChange = true;
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    internalScrollChange = false;
    layoutMaterialized();
    scheduleSync(RequestReason::EnsureVisible);
}

bool LongListWidget::lockViewportToItem(int index,
                                        Alignment alignment,
                                        int quietPeriodMs)
{
    if (index < 0 || index >= logicalCount) {
        clearViewportLock();
        return false;
    }

    // Replace any previous lock without reporting an intermediate unlock to the
    // domain layer. Apply alignment once, then preserve the resulting item-top
    // coordinate rather than repeatedly re-applying Center/Top/Bottom.
    releaseViewportLock(false);
    scrollToIndex(index, alignment);

    viewportLock.index = index;
    viewportLock.quietPeriodMs = std::max(0, quietPeriodMs);
    captureViewportLockFraction();
    touchViewportLock();
    return true;
}

bool LongListWidget::remapViewportLockedItem(int index)
{
    if (!hasViewportLock() || index < 0 || index >= logicalCount) {
        return false;
    }
    if (viewportLock.index == index) {
        touchViewportLock();
        return true;
    }

    viewportLock.index = index;
    QSignalBlocker blocker(verticalScrollBar());
    viewport()->setUpdatesEnabled(false);
    restoreViewportLock();
    layoutMaterialized();
    viewport()->setUpdatesEnabled(true);
    viewport()->update();
    touchViewportLock();
    scheduleSync(RequestReason::EnsureVisible);
    return true;
}

void LongListWidget::clearViewportLock()
{
    releaseViewportLock(true);
}

void LongListWidget::destroyItemWidget(int, QWidget* widget)
{
    if (widget) {
        widget->hide();
        widget->deleteLater();
    }
}

int LongListWidget::estimatedItemHeight(int) const
{
    return defaultHeight;
}

bool LongListWidget::eventFilter(QObject* watched, QEvent* event)
{
    const auto it = widgetIndexes.constFind(watched);
    if (it != widgetIndexes.cend()
        && (event->type() == QEvent::LayoutRequest || event->type() == QEvent::Resize)) {
        if (!synchronizing && !committingGeometry) {
            scheduleGeometryCommit(it.value());
        }
    }
    return QAbstractScrollArea::eventFilter(watched, event);
}

void LongListWidget::resizeEvent(QResizeEvent* event)
{
    const ViewAnchor anchor = captureAnchor();
    QAbstractScrollArea::resizeEvent(event);

    for (auto it = materialized.cbegin(); it != materialized.cend(); ++it) {
        dirtyGeometry.insert(it.key());
    }

    const qint64 oldOffset = contentOffset();
    QSignalBlocker blocker(verticalScrollBar());
    viewport()->setUpdatesEnabled(false);
    committingGeometry = true;
    for (int index : std::as_const(dirtyGeometry)) {
        if (QWidget* widget = materialized.value(index, nullptr)) {
            measureWidget(index, widget);
        }
    }
    dirtyGeometry.clear();
    updateScrollBarRange(oldOffset);
    if (hasViewportLock()) {
        // viewportLock.itemTopFraction was captured against the old viewport.
        // Using it with the new viewport height intentionally scales the locked
        // item's screen Y (e.g. 500/1000 -> 250/500).
        restoreViewportLock();
    } else {
        restoreAnchor(anchor);
    }
    layoutMaterialized();
    committingGeometry = false;
    viewport()->setUpdatesEnabled(true);
    viewport()->update();

    scheduleSync(RequestReason::Scroll);
}

void LongListWidget::wheelEvent(QWheelEvent* event)
{
    clearSeek();
    const QPoint pixelDelta = event->pixelDelta();
    int delta = pixelDelta.y();
    if (delta == 0) {
        const QPoint angleDelta = event->angleDelta();
        delta = angleDelta.y() / 120 * std::max(24, defaultHeight / 2);
    }

    if (delta != 0) {
        wheelInProgress = true;
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta);
        wheelInProgress = false;
        event->accept();
        scheduleSync(RequestReason::Scroll);
        noteUserViewportChange();
        return;
    }
    QAbstractScrollArea::wheelEvent(event);
}

void LongListWidget::scheduleSync(RequestReason reason)
{
    if (reason == RequestReason::Seek
        || pendingSyncReason == RequestReason::Initial
        || reason == RequestReason::EnsureVisible) {
        pendingSyncReason = reason;
    } else if (pendingSyncReason != RequestReason::Seek) {
        pendingSyncReason = reason;
    }
    if (!syncTimer.isActive()) {
        syncTimer.start(0);
    }
}

void LongListWidget::synchronize()
{
    if (synchronizing || logicalCount <= 0) {
        emitRangeChanges();
        return;
    }

    synchronizing = true;
    const RequestReason reason = seekActive ? RequestReason::Seek : pendingSyncReason;
    pendingSyncReason = RequestReason::Scroll;
    Range desired = seekActive ? desiredRangeForSeek() : desiredRangeForViewport();
    const int preferredCenter = seekActive && seekTarget >= 0
        ? seekTarget
        : (desired.isValid() ? (desired.first + desired.last) / 2 : -1);
    desired = clampToBudget(desired, preferredCenter);
    synchronizeRange(desired, reason, seekActive ? seekGeneration : 0, seekActive);
    synchronizing = false;
}

void LongListWidget::synchronizeRange(const Range& desired,
                                      RequestReason reason,
                                      quint64 generation,
                                      bool centerSeekTarget)
{
    if (!desired.isValid()) {
        emitRangeChanges();
        return;
    }

    const ViewAnchor anchor = captureAnchor();
    const qint64 oldOffset = contentOffset();
    QSignalBlocker blocker(verticalScrollBar());
    viewport()->setUpdatesEnabled(false);

    materializeAvailable(desired);
    evictOutside(desired, centerSeekTarget ? seekTarget : (desired.first + desired.last) / 2);
    updateScrollBarRange(oldOffset);
    if (centerSeekTarget && seekTarget >= 0 && materialized.contains(seekTarget)) {
        restoreSeekTarget();
    } else if (hasViewportLock()) {
        restoreViewportLock();
    } else {
        restoreAnchor(anchor);
    }
    layoutMaterialized();

    viewport()->setUpdatesEnabled(true);
    viewport()->update();

    requestMissing(desired, reason, generation);
    emitRangeChanges();
}

LongListWidget::Range LongListWidget::desiredRangeForViewport() const
{
    Range result;
    if (logicalCount <= 0 || viewport()->height() <= 0) {
        return result;
    }

    const qint64 top = contentOffset();
    const qint64 buffer = static_cast<qint64>(viewport()->height()) * bufferScreens;
    const qint64 firstPixel = std::max<qint64>(0, top - buffer);
    const qint64 lastPixel = std::min<qint64>(heights.totalHeight() - 1,
        top + viewport()->height() + buffer - 1);
    result.first = heights.indexAtPixel(firstPixel);
    result.last = heights.indexAtPixel(std::max(firstPixel, lastPixel));

    // The pixel/screen buffer is not enough by itself: a few unusually tall
    // items can shrink it to only one or two logical rows. Keep a hard minimum
    // logical look-ahead so an adjacent unavailable range is requested before
    // the user can scroll into it. With five items, a request starts as soon as
    // fewer than five concrete rows remain between the viewport and the gap.
    const Range visible = visibleRange();
    if (visible.isValid()) {
        result.first = std::min(result.first,
                                std::max(0, visible.first - MinimumPrefetchItems));
        result.last = std::max(result.last,
                               std::min(logicalCount - 1,
                                        visible.last + MinimumPrefetchItems));
    }
    return result;
}

LongListWidget::Range LongListWidget::desiredRangeForSeek() const
{
    if (!seekActive || seekTarget < 0 || logicalCount <= 0) {
        return {};
    }

    if (!materialized.contains(seekTarget)) {
        return boundedAround(seekTarget, blockSize);
    }

    const qint64 targetTop = heights.prefixHeight(seekTarget);
    const qint64 targetCenter = targetTop + heights.value(seekTarget) / 2;
    const qint64 radius = static_cast<qint64>(viewport()->height())
        * (2 * bufferScreens + 1) / 2;
    const qint64 firstPixel = std::max<qint64>(0, targetCenter - radius);
    const qint64 lastPixel = std::min<qint64>(heights.totalHeight() - 1,
        targetCenter + radius);

    Range result;
    result.first = heights.indexAtPixel(firstPixel);
    result.last = heights.indexAtPixel(lastPixel);
    const Range minimum = boundedAround(seekTarget, blockSize);
    if (minimum.isValid()) {
        result.first = std::min(result.first, minimum.first);
        result.last = std::max(result.last, minimum.last);
    }
    return result;
}

LongListWidget::Range LongListWidget::boundedAround(int center, int count) const
{
    Range result;
    if (logicalCount <= 0 || center < 0 || center >= logicalCount || count <= 0) {
        return result;
    }
    count = std::min(count, logicalCount);
    int first = center - (count - 1) / 2;
    first = std::max(0, std::min(first, logicalCount - count));
    result.first = first;
    result.last = first + count - 1;
    return result;
}

LongListWidget::Range LongListWidget::clampToBudget(const Range& range, int preferredCenter) const
{
    if (!range.isValid() || range.count() <= maxMaterializedItems) {
        return range;
    }
    preferredCenter = std::max(range.first, std::min(range.last, preferredCenter));
    Range result = boundedAround(preferredCenter, maxMaterializedItems);
    result.first = std::max(result.first, range.first);
    result.last = std::min(result.last, range.last);
    return result;
}

void LongListWidget::materializeAvailable(const Range& range)
{
    for (int index = range.first; index <= range.last; ++index) {
        if (!available.testBit(index) || materialized.contains(index)) {
            continue;
        }

        QWidget* widget = createItemWidget(index);
        if (!widget) {
            continue;
        }
        widget->setParent(viewport());
        widget->installEventFilter(this);
        widgetIndexes.insert(widget, index);
        materialized.insert(index, widget);
        widget->show();

        const int width = std::max(1, viewport()->width());
        widget->resize(width, std::max(1, heights.value(index)));
        if (QLayout* layout = widget->layout()) {
            layout->activate();
        }
        measureWidget(index, widget);
    }
}

void LongListWidget::evictOutside(const Range& keepRange, int preferredCenter)
{
    Q_UNUSED(preferredCenter)
    const QVector<int> current = materializedIndices();
    for (int index : current) {
        if (keepRange.contains(index)) {
            continue;
        }
        QWidget* widget = materialized.take(index);
        if (!widget) {
            continue;
        }
        widgetIndexes.remove(widget);
        widget->removeEventFilter(this);
        destroyItemWidget(index, widget);
    }
}

void LongListWidget::layoutMaterialized()
{
    if (materialized.isEmpty()) {
        return;
    }

    const qint64 top = contentOffset();
    const int width = std::max(1, viewport()->width());
    for (auto it = materialized.begin(); it != materialized.end(); ++it) {
        const int index = it.key();
        QWidget* widget = it.value();
        if (!widget) {
            continue;
        }
        const qint64 y64 = heights.prefixHeight(index) - top;
        const int y = static_cast<int>(std::max<qint64>(INT_MIN,
            std::min<qint64>(INT_MAX, y64)));
        widget->setGeometry(0, y, width, heights.value(index));
    }
}

void LongListWidget::measureWidget(int index, QWidget* widget)
{
    if (!widget || index < 0 || index >= logicalCount) {
        return;
    }

    const int width = std::max(1, viewport()->width());
    if (widget->width() != width) {
        widget->resize(width, std::max(1, widget->height()));
    }
    if (QLayout* layout = widget->layout()) {
        layout->activate();
    }
    widget->updateGeometry();

    const int heightForWidth = widget->heightForWidth(width);
    const int height = std::max({
        1,
        heightForWidth,
        widget->sizeHint().height(),
        widget->minimumSizeHint().height(),
    });
    heights.setValue(index, height);
    measured.setBit(index, true);
}

void LongListWidget::scheduleGeometryCommit(int index)
{
    if (index >= 0) {
        dirtyGeometry.insert(index);
    }
    if (!geometryTimer.isActive()) {
        geometryTimer.start(0);
    }
}

void LongListWidget::commitGeometry()
{
    if (committingGeometry || synchronizing) {
        if (!geometryTimer.isActive()) {
            geometryTimer.start(0);
        }
        return;
    }

    const ViewAnchor anchor = captureAnchor();
    const qint64 oldOffset = contentOffset();
    QSignalBlocker blocker(verticalScrollBar());
    viewport()->setUpdatesEnabled(false);
    committingGeometry = true;

    for (int index : std::as_const(dirtyGeometry)) {
        if (QWidget* widget = materialized.value(index, nullptr)) {
            measureWidget(index, widget);
        }
    }
    dirtyGeometry.clear();

    updateScrollBarRange(oldOffset);
    if (seekActive && seekTarget >= 0 && materialized.contains(seekTarget)) {
        restoreSeekTarget();
    } else if (hasViewportLock()) {
        restoreViewportLock();
    } else {
        restoreAnchor(anchor);
    }
    layoutMaterialized();

    committingGeometry = false;
    viewport()->setUpdatesEnabled(true);
    viewport()->update();

    scheduleSync(seekActive ? RequestReason::Seek : RequestReason::Scroll);
    emitRangeChanges();
}

void LongListWidget::requestMissing(const Range& desired,
                                    RequestReason reason,
                                    quint64 generation)
{
    if (!desired.isValid()) {
        return;
    }

    int index = desired.first;
    while (index <= desired.last) {
        if (available.testBit(index) || pendingRequest.testBit(index)) {
            ++index;
            continue;
        }

        const int requestFirst = std::max(0, (index / blockSize) * blockSize);
        const int requestLast = std::min(logicalCount - 1, requestFirst + blockSize - 1);
        bool hasMissing = false;
        for (int current = requestFirst; current <= requestLast; ++current) {
            if (!available.testBit(current) && !pendingRequest.testBit(current)) {
                pendingRequest.setBit(current, true);
                hasMissing = true;
            }
        }
        if (hasMissing) {
            emit rangeRequested(requestFirst, requestLast, reason, generation);
        }
        index = requestLast + 1;
    }
}

void LongListWidget::clearPendingRequest(int first, int last)
{
    first = std::max(0, first);
    last = std::min(logicalCount - 1, last);
    for (int index = first; index <= last; ++index) {
        pendingRequest.clearBit(index);
    }
}

LongListWidget::ViewAnchor LongListWidget::captureAnchor() const
{
    ViewAnchor anchor;
    if (logicalCount <= 0) {
        return anchor;
    }

    const qint64 offset = contentOffset();
    if (maximumContentOffset() - offset <= 2) {
        anchor.kind = ViewAnchor::Bottom;
        return anchor;
    }

    const int index = heights.indexAtPixel(offset);
    if (index < 0) {
        return anchor;
    }
    anchor.kind = ViewAnchor::Item;
    anchor.index = index;
    anchor.offsetInsideItem = offset - heights.prefixHeight(index);
    return anchor;
}

void LongListWidget::restoreAnchor(const ViewAnchor& anchor)
{
    qint64 offset = contentOffset();
    if (anchor.kind == ViewAnchor::Bottom) {
        offset = maximumContentOffset();
    } else if (anchor.kind == ViewAnchor::Item
               && anchor.index >= 0 && anchor.index < logicalCount) {
        offset = heights.prefixHeight(anchor.index) + anchor.offsetInsideItem;
    }
    verticalScrollBar()->setValue(scrollValueForContentOffset(offset));
}

void LongListWidget::restoreSeekTarget()
{
    if (!seekActive || seekTarget < 0 || seekTarget >= logicalCount) {
        return;
    }
    const qint64 targetCenter = heights.prefixHeight(seekTarget)
        + heights.value(seekTarget) / 2;
    const qint64 offset = targetCenter - viewport()->height() / 2;
    verticalScrollBar()->setValue(scrollValueForContentOffset(offset));
}

void LongListWidget::captureViewportLockFraction()
{
    if (!hasViewportLock() || viewportLock.index >= logicalCount) {
        return;
    }
    const int viewportHeight = std::max(1, viewport()->height());
    const qint64 itemTopY = heights.prefixHeight(viewportLock.index) - contentOffset();
    viewportLock.itemTopFraction = static_cast<long double>(itemTopY)
        / static_cast<long double>(viewportHeight);
}

void LongListWidget::restoreViewportLock()
{
    if (!hasViewportLock() || viewportLock.index >= logicalCount) {
        return;
    }
    const long double desiredViewportY = viewportLock.itemTopFraction
        * static_cast<long double>(std::max(1, viewport()->height()));
    const qint64 targetOffset = heights.prefixHeight(viewportLock.index)
        - static_cast<qint64>(std::llround(desiredViewportY));
    verticalScrollBar()->setValue(scrollValueForContentOffset(targetOffset));
}

void LongListWidget::touchViewportLock()
{
    if (!hasViewportLock()) {
        return;
    }
    viewportLockTimer.stop();
    if (viewportLock.quietPeriodMs > 0) {
        viewportLockTimer.start(viewportLock.quietPeriodMs);
    }
}

void LongListWidget::releaseViewportLock(bool notify)
{
    const bool wasLocked = hasViewportLock();
    viewportLockTimer.stop();
    viewportLock = ViewportLock();
    if (wasLocked && notify) {
        emit viewportLockReleased();
    }
}

void LongListWidget::noteUserViewportChange()
{
    releaseViewportLock(true);
    emit userViewportChanged(isAtEnd());
}

qint64 LongListWidget::maximumContentOffset() const
{
    return std::max<qint64>(0, heights.totalHeight() - viewport()->height());
}

qint64 LongListWidget::contentOffset() const
{
    return contentOffsetForScrollValue(verticalScrollBar()->value());
}

int LongListWidget::scrollValueForContentOffset(qint64 offset) const
{
    const qint64 maximumOffset = maximumContentOffset();
    const int barMaximum = verticalScrollBar()->maximum();
    if (maximumOffset <= 0 || barMaximum <= 0) {
        return 0;
    }
    offset = std::max<qint64>(0, std::min(offset, maximumOffset));
    if (maximumOffset <= INT_MAX) {
        return static_cast<int>(offset);
    }
    const long double fraction = static_cast<long double>(offset)
        / static_cast<long double>(maximumOffset);
    return static_cast<int>(std::llround(fraction * barMaximum));
}

qint64 LongListWidget::contentOffsetForScrollValue(int value) const
{
    const qint64 maximumOffset = maximumContentOffset();
    const int barMaximum = verticalScrollBar()->maximum();
    if (maximumOffset <= 0 || barMaximum <= 0) {
        return 0;
    }
    value = std::max(0, std::min(value, barMaximum));
    if (maximumOffset <= INT_MAX) {
        return value;
    }
    const long double fraction = static_cast<long double>(value)
        / static_cast<long double>(barMaximum);
    return static_cast<qint64>(std::llround(fraction * maximumOffset));
}

void LongListWidget::updateScrollBarRange(qint64 preservedOffset)
{
    const qint64 maximumOffset = maximumContentOffset();
    const int maximum = static_cast<int>(std::min<qint64>(maximumOffset, INT_MAX));
    QScrollBar* bar = verticalScrollBar();
    bar->setRange(0, maximum);

    if (maximumOffset <= INT_MAX) {
        bar->setPageStep(std::max(1, viewport()->height()));
        bar->setSingleStep(std::max(1, defaultHeight / 3));
    } else {
        const long double scale = static_cast<long double>(maximum)
            / static_cast<long double>(maximumOffset);
        bar->setPageStep(std::max(1,
            static_cast<int>(std::llround(viewport()->height() * scale))));
        bar->setSingleStep(std::max(1,
            static_cast<int>(std::llround((defaultHeight / 3.0L) * scale))));
    }
    bar->setValue(scrollValueForContentOffset(preservedOffset));
}

void LongListWidget::onScrollValueChanged()
{
    layoutMaterialized();
    if (internalScrollChange || committingGeometry || synchronizing) {
        return;
    }
    if (verticalScrollBar()->isSliderDown()) {
        return;
    }
    scheduleSync(RequestReason::Scroll);
}

void LongListWidget::onSliderMoved(int value)
{
    // sliderMoved is emitted only for interactive thumb movement. The first
    // move immediately gives the user authority over the viewport, even before
    // sliderReleased publishes the final viewport state.
    releaseViewportLock(true);

    const int target = logicalTargetForScrollValue(value);
    if (target < 0) {
        return;
    }
    if (!seekActive || target != seekTarget) {
        ++seekGeneration;
        seekTarget = target;
    }
    seekActive = true;
    seekTimer.start(seekDebounceInterval);
}

void LongListWidget::activateSeekTarget()
{
    if (!seekActive || seekTarget < 0) {
        return;
    }
    scheduleSync(RequestReason::Seek);
}

int LongListWidget::logicalTargetForScrollValue(int value) const
{
    if (logicalCount <= 0) {
        return -1;
    }
    const QScrollBar* bar = verticalScrollBar();
    if (logicalCount == 1 || bar->maximum() <= bar->minimum()) {
        return 0;
    }
    value = std::max(bar->minimum(), std::min(value, bar->maximum()));
    const long double fraction = static_cast<long double>(value - bar->minimum())
        / static_cast<long double>(bar->maximum() - bar->minimum());
    return std::max(0, std::min(logicalCount - 1,
        static_cast<int>(std::llround(fraction * (logicalCount - 1)))));
}

void LongListWidget::clearSeek()
{
    if (seekActive || seekTarget >= 0) {
        ++seekGeneration;
    }
    seekActive = false;
    seekTarget = -1;
    seekTimer.stop();
}

void LongListWidget::emitRangeChanges()
{
    const Range visible = visibleRange();
    if (!sameRange(visible, lastVisibleRange)) {
        lastVisibleRange = visible;
        emit visibleRangeChanged(visible.first, visible.last);
    }

    const Range concrete = materializedRange();
    if (!sameRange(concrete, lastMaterializedRange)) {
        lastMaterializedRange = concrete;
        emit materializedRangeChanged(concrete.first, concrete.last);
    }
}

} // namespace Mattermost
