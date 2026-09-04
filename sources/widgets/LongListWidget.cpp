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

bool sameRange(const LongListWidget::Range& lhs, const LongListWidget::Range& rhs)
{
    return lhs.first == rhs.first && lhs.last == rhs.last;
}

int boundedHeight(int height)
{
    return std::max(1, height);
}

} // namespace

void LongListWidget::HeightIndex::reset(int count, int height)
{
    count = std::max(0, count);
    height = boundedHeight(height);
    values.fill(height, count);
    fenwick.fill(0, count + 1);
    for (int index = 0; index < count; ++index) {
        add(index, height);
    }
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

    QScrollBar* bar = verticalScrollBar();
    connect(bar, &QScrollBar::valueChanged, this, [this] { onScrollValueChanged(); });
    connect(bar, &QScrollBar::sliderMoved, this, &LongListWidget::onSliderMoved);
    connect(bar, &QScrollBar::sliderReleased, this, [this, bar] {
        onSliderMoved(bar->value());
        seekTimer.stop();
        activateSeekTarget();
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
    // resizing the height index. A previously empty list has no user viewport
    // intent, so initial population remains controlled by the caller.
    const bool preserveBottom = logicalCount > 0 && isAtEnd();

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

    commitGeometry();
    if (preserveBottom && logicalCount > 0) {
        scrollToEnd();
    } else {
        scheduleSync(RequestReason::Initial);
    }
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
        }
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
    scheduleSync(RequestReason::EnsureVisible);
}

void LongListWidget::scrollToEnd()
{
    clearSeek();
    internalScrollChange = true;
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    internalScrollChange = false;
    layoutMaterialized();
    scheduleSync(RequestReason::EnsureVisible);
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
    restoreAnchor(anchor);
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
