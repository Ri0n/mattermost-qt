#pragma once

#include <QAbstractScrollArea>
#include <QBitArray>
#include <QHash>
#include <QSet>
#include <QTimer>
#include <QVector>

namespace Mattermost {

/**
 * A virtualized widget list for very long, partially available sequences.
 *
 * LongListWidget owns all scroll geometry, materialization and viewport anchoring.
 * There are no placeholder/gap widgets: unavailable or unmaterialized items are
 * represented only by their estimated heights in the internal height index.
 *
 * Subclasses provide concrete item widgets through createItemWidget(). Data
 * availability is reported independently with setRangeAvailable(). When a range
 * required by the viewport is unavailable, rangeRequested() is emitted.
 */
class LongListWidget : public QAbstractScrollArea
{
    Q_OBJECT
public:
    enum class RequestReason {
        Initial,
        Scroll,
        Seek,
        EnsureVisible,
    };
    Q_ENUM(RequestReason)

    enum class Alignment {
        EnsureVisible,
        Top,
        Center,
        Bottom,
    };
    Q_ENUM(Alignment)

    struct Range {
        int first = -1;
        int last = -1;

        bool isValid() const { return first >= 0 && last >= first; }
        int count() const { return isValid() ? last - first + 1 : 0; }
        bool contains(int index) const
        {
            return isValid() && index >= first && index <= last;
        }
    };

    explicit LongListWidget(QWidget* parent = nullptr);
    ~LongListWidget() override;

    int itemCount() const { return logicalCount; }
    void setItemCount(int count);

    /** Insert real logical items and preserve the current semantic viewport anchor. */
    void insertItems(int first, int count);

    int defaultItemHeight() const { return defaultHeight; }
    void setDefaultItemHeight(int height);

    int materializationLimit() const { return maxMaterializedItems; }
    void setMaterializationLimit(int count);

    int requestBlockSize() const { return blockSize; }
    void setRequestBlockSize(int count);

    int prefetchScreens() const { return bufferScreens; }
    void setPrefetchScreens(int screens);

    int seekDebounceMs() const { return seekDebounceInterval; }
    void setSeekDebounceMs(int milliseconds);

    void setRangeAvailable(int first, int last, bool available = true);
    bool isItemAvailable(int index) const;

    /**
     * A data source must finish every range request even when it failed or
     * returned a differently aligned server page. This releases request
     * suppression; it intentionally does not reschedule immediately, so a
     * network failure cannot turn into a tight retry loop.
     */
    void finishRangeRequest(int first, int last)
    {
        clearPendingRequest(first, last);
    }

    /** Notify the view that data for already available items changed. */
    void itemsChanged(int first, int last);

    QWidget* itemWidget(int index) const;
    QVector<int> materializedIndices() const;
    int materializedCount() const { return materialized.size(); }

    Range visibleRange() const;
    Range materializedRange() const;

    qint64 contentHeight() const;
    int indexAtViewportPosition(int viewportY) const;
    bool isAtEnd() const
    {
        return maximumContentOffset() - contentOffset() <= 2;
    }

    void scrollToIndex(int index, Alignment alignment = Alignment::EnsureVisible);
    void scrollToEnd();

signals:
    /**
     * Request logical items. first/last are inclusive. generation changes for
     * every new random-seek target; ordinary scroll requests use generation 0.
     */
    void rangeRequested(int first,
                        int last,
                        Mattermost::LongListWidget::RequestReason reason,
                        quint64 generation);
    void visibleRangeChanged(int first, int last);
    void materializedRangeChanged(int first, int last);

    /** Emitted only for direct user scrollbar/wheel movement. */
    void userViewportChanged(bool atEnd);

protected:
    /** Return a new widget for an available logical item. Ownership is transferred. */
    virtual QWidget* createItemWidget(int index) = 0;

    /** Default destruction policy for an evicted materialized widget. */
    virtual void destroyItemWidget(int index, QWidget* widget);

    /** Override when some unmeasured items have a better per-index estimate. */
    virtual int estimatedItemHeight(int index) const;

    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    class HeightIndex
    {
    public:
        void reset(int count, int height);
        void resize(int count, int height);
        void insert(int first, int count, int height);
        int size() const { return static_cast<int>(values.size()); }
        int value(int index) const;
        void setValue(int index, int value);
        qint64 prefixHeight(int count) const;
        qint64 totalHeight() const { return prefixHeight(static_cast<int>(values.size())); }
        int indexAtPixel(qint64 pixel) const;

    private:
        void add(int index, qint64 delta);
        void rebuildFenwick();

        QVector<int> values;
        QVector<qint64> fenwick;
    };

    struct ViewAnchor {
        enum Kind {
            Invalid,
            Item,
            Bottom,
        } kind = Invalid;
        int index = -1;
        qint64 offsetInsideItem = 0;
    };

    void scheduleSync(RequestReason reason);
    void synchronize();
    void synchronizeRange(const Range& desired,
                          RequestReason reason,
                          quint64 generation,
                          bool centerSeekTarget);

    Range desiredRangeForViewport() const;
    Range desiredRangeForSeek() const;
    Range boundedAround(int center, int count) const;
    Range clampToBudget(const Range& range, int preferredCenter) const;

    void materializeAvailable(const Range& range);
    void evictOutside(const Range& keepRange, int preferredCenter);
    void layoutMaterialized();
    void measureWidget(int index, QWidget* widget);
    void scheduleGeometryCommit(int index = -1);
    void commitGeometry();

    void requestMissing(const Range& desired,
                        RequestReason reason,
                        quint64 generation);
    void clearPendingRequest(int first, int last);

    ViewAnchor captureAnchor() const;
    void restoreAnchor(const ViewAnchor& anchor);
    void restoreSeekTarget();

    qint64 maximumContentOffset() const;
    qint64 contentOffset() const;
    int scrollValueForContentOffset(qint64 offset) const;
    qint64 contentOffsetForScrollValue(int value) const;
    void updateScrollBarRange(qint64 preservedOffset);

    void onScrollValueChanged();
    void onSliderMoved(int value);
    void activateSeekTarget();
    int logicalTargetForScrollValue(int value) const;
    void clearSeek();

    void emitRangeChanges();

    int logicalCount = 0;
    int defaultHeight = 96;
    int maxMaterializedItems = 200;
    int blockSize = 10;
    int bufferScreens = 1;
    int seekDebounceInterval = 100;

    HeightIndex heights;
    QBitArray measured;
    QBitArray available;
    QBitArray pendingRequest;

    QHash<int, QWidget*> materialized;
    QHash<QObject*, int> widgetIndexes;
    QSet<int> dirtyGeometry;

    QTimer syncTimer;
    QTimer geometryTimer;
    QTimer seekTimer;
    RequestReason pendingSyncReason = RequestReason::Initial;

    bool synchronizing = false;
    bool committingGeometry = false;
    bool internalScrollChange = false;
    bool wheelInProgress = false;

    quint64 seekGeneration = 0;
    int seekTarget = -1;
    bool seekActive = false;

    Range lastVisibleRange;
    Range lastMaterializedRange;
};

} // namespace Mattermost
