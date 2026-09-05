#include "ChatLogWidget.h"

#include <algorithm>

#include <QLoggingCategory>
#include <QTimer>

#include "ChatArea.h"
#include "backend/Backend.h"
#include "backend/types/BackendPost.h"
#include "post/PostWidget.h"

namespace Mattermost {

namespace {

Q_LOGGING_CATEGORY(lcTimelineTrace, "mattermost.timeline.trace", QtWarningMsg)

const char* requestReasonName(LongListWidget::RequestReason reason)
{
    switch (reason) {
    case LongListWidget::RequestReason::Initial:
        return "initial";
    case LongListWidget::RequestReason::Scroll:
        return "scroll";
    case LongListWidget::RequestReason::Seek:
        return "seek";
    case LongListWidget::RequestReason::EnsureVisible:
        return "ensure-visible";
    }
    return "unknown";
}

const char* sourceName(const AbstractPostSource* source)
{
    return source ? source->metaObject()->className() : "none";
}

} // namespace

ChatLogWidget::ChatLogWidget(QWidget* parent)
    : LongListWidget(parent)
{
    setDefaultItemHeight(96);
    setMaterializationLimit(200);
    setRequestBlockSize(10);
    setPrefetchScreens(1);
    setSeekDebounceMs(100);

    connect(this, &LongListWidget::rangeRequested, this,
            [this](int first, int last, RequestReason reason, quint64 generation) {
        const Range visible = visibleRange();
        const Range concrete = materializedRange();
        qCDebug(lcTimelineTrace).nospace()
            << "RANGE_REQUEST list=" << static_cast<const void*>(this)
            << " source=" << sourceName(postSource)
            << " sourcePtr=" << static_cast<const void*>(postSource)
            << " requested=[" << first << ',' << last << ']'
            << " reason=" << requestReasonName(reason)
            << " generation=" << generation
            << " itemCount=" << itemCount()
            << " visible=[" << visible.first << ',' << visible.last << ']'
            << " materialized=[" << concrete.first << ',' << concrete.last << ']'
            << " materializedCount=" << materializedCount();

        if (!postSource) {
            finishRangeRequest(first, last);
            return;
        }
        postSource->requestRange(first, last, toSourceReason(reason), generation);
    });

    connect(this, &LongListWidget::visibleRangeChanged, this,
            [this](int first, int last) {
        qCDebug(lcTimelineTrace).nospace()
            << "VISIBLE_RANGE list=" << static_cast<const void*>(this)
            << " source=" << sourceName(postSource)
            << " range=[" << first << ',' << last << ']'
            << " itemCount=" << itemCount();

        if (!postSource || first < 0 || first > 1 || !postSource->canRequestBeforeFirst()) {
            return;
        }
        postSource->requestBeforeFirst(AbstractPostSource::RequestReason::Scroll, 0);
    });

    connect(this, &LongListWidget::materializedRangeChanged, this,
            [this](int first, int last) {
        qCDebug(lcTimelineTrace).nospace()
            << "MATERIALIZED_RANGE list=" << static_cast<const void*>(this)
            << " source=" << sourceName(postSource)
            << " range=[" << first << ',' << last << ']'
            << " count=" << materializedCount();
    });

    // LongListWidget owns the lock lifetime and recognizes all real user scroll
    // gestures. ChatLogWidget only drops the corresponding semantic post ID.
    connect(this, &LongListWidget::viewportLockReleased, this, [this] {
        qCDebug(lcTimelineTrace).nospace()
            << "VIEWPORT_LOCK_RELEASED list=" << static_cast<const void*>(this)
            << " source=" << sourceName(postSource)
            << " postId=" << navigationPostId
            << " index=" << navigationLogicalIndex;
        navigationPostId.clear();
        navigationLogicalIndex = -1;
    });
}

ChatLogWidget::~ChatLogWidget()
{
    for (const QMetaObject::Connection& connection : sourceConnections) {
        disconnect(connection);
    }
}

void ChatLogWidget::configure(Backend& backendInstance, ChatArea& chatAreaInstance)
{
    backend = &backendInstance;
    chatArea = &chatAreaInstance;
}

void ChatLogWidget::setSource(AbstractPostSource* sourceInstance)
{
    if (postSource == sourceInstance) {
        return;
    }

    qCDebug(lcTimelineTrace).nospace()
        << "SET_SOURCE list=" << static_cast<const void*>(this)
        << " old=" << sourceName(postSource)
        << " oldPtr=" << static_cast<const void*>(postSource)
        << " new=" << sourceName(sourceInstance)
        << " newPtr=" << static_cast<const void*>(sourceInstance)
        << " newCount=" << (sourceInstance ? sourceInstance->itemCount() : 0);

    clearNavigationLock();
    for (const QMetaObject::Connection& connection : sourceConnections) {
        disconnect(connection);
    }
    sourceConnections.clear();
    postSource = sourceInstance;

    setItemCount(postSource ? postSource->itemCount() : 0);
    if (!postSource) {
        return;
    }

    reconnectSource();
    for (int index = 0; index < postSource->itemCount(); ++index) {
        if (postSource->isAvailable(index)) {
            setRangeAvailable(index, index, true);
        }
    }
}

PostWidget* ChatLogWidget::findPost(const QString& postId) const
{
    if (!postSource || postId.isEmpty()) {
        return nullptr;
    }
    const int index = postSource->indexOfPost(postId);
    return index >= 0 ? qobject_cast<PostWidget*>(itemWidget(index)) : nullptr;
}

bool ChatLogWidget::ensurePostVisible(const QString& postId, Alignment alignment)
{
    if (!postSource || postId.isEmpty()) {
        return false;
    }
    const int index = postSource->ensurePostIndex(postId);
    if (index < 0) {
        return false;
    }

    // Once semantic navigation established a viewport lock, repeated
    // ensure/go-to calls must not re-apply Center/Top and move the post again.
    // Only an authoritative identity remap updates the locked logical index.
    if (navigationPostId == postId && hasViewportLock()) {
        if (index != navigationLogicalIndex) {
            if (!remapViewportLockedItem(index)) {
                return false;
            }
            navigationLogicalIndex = index;
        }
        return true;
    }

    scrollToIndex(index, alignment);
    return true;
}

void ChatLogWidget::highlightPost(const QString& postId)
{
    if (PostWidget* widget = findPost(postId)) {
        widget->setFocus(Qt::OtherFocusReason);
        widget->update();
    }
}

void ChatLogWidget::refreshPost(const QString& postId)
{
    if (!postSource || postId.isEmpty()) {
        return;
    }
    const int index = postSource->indexOfPost(postId);
    if (index >= 0) {
        rematerializeRange(index, index);
    }
}

void ChatLogWidget::followOwnPost(const QString& postId)
{
    // This policy belongs to the Mattermost-specific list, not to the generic
    // LongListWidget: a locally confirmed outgoing post means the user wants the
    // live edge even if the list was not considered sticky-bottom a moment ago.
    // Defer one event-loop turn so source insertion and availability signals have
    // completed before the final content height is used.
    QTimer::singleShot(0, this, [this, postId] {
        if (!postSource || postId.isEmpty() || postSource->indexOfPost(postId) < 0) {
            return;
        }
        qCDebug(lcTimelineTrace).nospace()
            << "FOLLOW_OWN_POST list=" << static_cast<const void*>(this)
            << " source=" << sourceName(postSource)
            << " postId=" << postId;
        clearNavigationLock();
        scrollToEnd();
    });
}

bool ChatLogWidget::lockNavigationToPost(const QString& postId,
                                         Alignment alignment,
                                         int quietPeriodMs)
{
    if (!postSource || postId.isEmpty()) {
        clearNavigationLock();
        return false;
    }

    const int index = postSource->ensurePostIndex(postId);
    if (index < 0) {
        clearNavigationLock();
        return false;
    }

    navigationPostId = postId;
    navigationLogicalIndex = index;
    if (!lockViewportToItem(index, alignment, quietPeriodMs)) {
        navigationPostId.clear();
        navigationLogicalIndex = -1;
        return false;
    }
    return true;
}

void ChatLogWidget::clearNavigationLock()
{
    navigationPostId.clear();
    navigationLogicalIndex = -1;
    clearViewportLock();
}

bool ChatLogWidget::editLastOwnPost()
{
    if (!postSource) {
        return false;
    }

    QVector<int> indices = materializedIndices();
    std::sort(indices.begin(), indices.end(), std::greater<int>());
    for (int index : indices) {
        BackendPost* post = postSource->postAt(index);
        if (!post || !post->isOwnPost()) {
            continue;
        }
        editedPostWidget = qobject_cast<PostWidget*>(itemWidget(index));
        if (editedPostWidget) {
            emit postEditInitiated(*post);
            return true;
        }
    }
    return false;
}

void ChatLogWidget::postEditFinished()
{
    editedPostWidget.clear();
}

QWidget* ChatLogWidget::createItemWidget(int index)
{
    if (!backend || !chatArea || !postSource) {
        return nullptr;
    }

    BackendPost* post = postSource->postAt(index);
    if (!post) {
        return nullptr;
    }

    BackendPost* lastRootPost = nullptr;
    if (index > 0) {
        if (BackendPost* previous = postSource->postAt(index - 1)) {
            lastRootPost = previous->rootPost;
        }
    }

    const QString postId = post->id;
    auto* widget = new PostWidget(*backend, *post, viewport(), chatArea, lastRootPost);
    qCDebug(lcTimelineTrace).nospace()
        << "CREATE_WIDGET list=" << static_cast<const void*>(this)
        << " source=" << sourceName(postSource)
        << " index=" << index
        << " postId=" << postId
        << " rootId=" << post->root_id
        << " widget=" << static_cast<const void*>(widget)
        << " sizeHint=" << widget->sizeHint().height()
        << " minHint=" << widget->minimumSizeHint().height();

    connect(widget, &PostWidget::dimensionsChanged, this, [this, postId, widget] {
        if (!postSource) {
            return;
        }
        const int currentIndex = postSource->indexOfPost(postId);
        qCDebug(lcTimelineTrace).nospace()
            << "DIMENSIONS_CHANGED list=" << static_cast<const void*>(this)
            << " source=" << sourceName(postSource)
            << " index=" << currentIndex
            << " postId=" << postId
            << " widget=" << static_cast<const void*>(widget)
            << " y=" << widget->y()
            << " height=" << widget->height()
            << " sizeHint=" << widget->sizeHint().height()
            << " minHint=" << widget->minimumSizeHint().height();
        if (currentIndex >= 0) {
            itemsChanged(currentIndex, currentIndex);
        }
    });
    return widget;
}

void ChatLogWidget::destroyItemWidget(int index, QWidget* widget)
{
    BackendPost* post = postSource ? postSource->postAt(index) : nullptr;
    qCDebug(lcTimelineTrace).nospace()
        << "DESTROY_WIDGET list=" << static_cast<const void*>(this)
        << " source=" << sourceName(postSource)
        << " index=" << index
        << " postId=" << (post ? post->id : QString())
        << " widget=" << static_cast<const void*>(widget)
        << " y=" << (widget ? widget->y() : 0)
        << " height=" << (widget ? widget->height() : 0);

    if (editedPostWidget == widget) {
        editedPostWidget.clear();
    }
    LongListWidget::destroyItemWidget(index, widget);
}

AbstractPostSource::RequestReason ChatLogWidget::toSourceReason(RequestReason reason)
{
    switch (reason) {
    case RequestReason::Initial:
        return AbstractPostSource::RequestReason::Initial;
    case RequestReason::Seek:
        return AbstractPostSource::RequestReason::Seek;
    case RequestReason::EnsureVisible:
        return AbstractPostSource::RequestReason::EnsureVisible;
    case RequestReason::Scroll:
    default:
        return AbstractPostSource::RequestReason::Scroll;
    }
}

void ChatLogWidget::reconnectSource()
{
    if (!postSource) {
        return;
    }

    sourceConnections.push_back(connect(postSource, &AbstractPostSource::itemCountChanged,
                                        this, [this](int count) {
        qCDebug(lcTimelineTrace).nospace()
            << "SOURCE_ITEM_COUNT list=" << static_cast<const void*>(this)
            << " source=" << sourceName(postSource)
            << " count=" << count;
        setItemCount(count);
        restoreNavigationTarget();
    }));
    sourceConnections.push_back(connect(postSource, &AbstractPostSource::itemsInserted,
                                        this, [this](int first, int count) {
        qCDebug(lcTimelineTrace).nospace()
            << "SOURCE_INSERTED list=" << static_cast<const void*>(this)
            << " source=" << sourceName(postSource)
            << " first=" << first
            << " count=" << count;
        insertItems(first, count);
        restoreNavigationTarget();
    }));
    sourceConnections.push_back(connect(postSource, &AbstractPostSource::itemsRemoved,
                                        this, [this](int first, int count) {
        qCDebug(lcTimelineTrace).nospace()
            << "SOURCE_REMOVED list=" << static_cast<const void*>(this)
            << " source=" << sourceName(postSource)
            << " first=" << first
            << " count=" << count;
        removeItems(first, count);
        restoreNavigationTarget();
    }));
    sourceConnections.push_back(connect(postSource, &AbstractPostSource::rangeAvailable,
                                        this, [this](int first, int last) {
        qCDebug(lcTimelineTrace).nospace()
            << "SOURCE_AVAILABLE list=" << static_cast<const void*>(this)
            << " source=" << sourceName(postSource)
            << " range=[" << first << ',' << last << ']';
        setRangeAvailable(first, last, true);
        restoreNavigationTarget();
    }));
    sourceConnections.push_back(connect(postSource, &AbstractPostSource::itemsChanged,
                                        this, [this](int first, int last) {
        qCDebug(lcTimelineTrace).nospace()
            << "SOURCE_CHANGED list=" << static_cast<const void*>(this)
            << " source=" << sourceName(postSource)
            << " range=[" << first << ',' << last << ']';
        rematerializeRange(first, last);
        restoreNavigationTarget();
    }));
    sourceConnections.push_back(connect(postSource, &AbstractPostSource::rangeRequestFinished,
                                        this, [this](int first, int last) {
        qCDebug(lcTimelineTrace).nospace()
            << "SOURCE_REQUEST_FINISHED list=" << static_cast<const void*>(this)
            << " source=" << sourceName(postSource)
            << " range=[" << first << ',' << last << ']';
        finishRangeRequest(first, last);
    }));
}

void ChatLogWidget::rematerializeRange(int first, int last)
{
    if (!postSource || itemCount() <= 0) {
        return;
    }
    first = std::max(0, first);
    last = std::min(itemCount() - 1, last);
    if (last < first) {
        return;
    }

    qCDebug(lcTimelineTrace).nospace()
        << "REMATERIALIZE_BEGIN list=" << static_cast<const void*>(this)
        << " source=" << sourceName(postSource)
        << " range=[" << first << ',' << last << ']';

    for (int index = first; index <= last; ++index) {
        const bool sourceAvailable = postSource->isAvailable(index);
        if (itemWidget(index)) {
            BackendPost* post = postSource->postAt(index);
            qCDebug(lcTimelineTrace).nospace()
                << "REMATERIALIZE_WIDGET list=" << static_cast<const void*>(this)
                << " source=" << sourceName(postSource)
                << " index=" << index
                << " available=" << sourceAvailable
                << " postId=" << (post ? post->id : QString());
            // Force replacement so PostWidget gets the source's new identity or
            // content rather than retaining an object for a provisional slot.
            setRangeAvailable(index, index, false);
            if (sourceAvailable) {
                setRangeAvailable(index, index, true);
            }
            continue;
        }

        // Availability itself can change before a QWidget was ever materialized
        // (notably when an estimated semantic target moves to an authoritative
        // page). Keep LongListWidget's bitset synchronized with the source too.
        setRangeAvailable(index, index, sourceAvailable);
    }
}

bool ChatLogWidget::restoreNavigationTarget()
{
    if (!postSource || navigationPostId.isEmpty() || !hasViewportLock()) {
        return false;
    }

    const int index = postSource->indexOfPost(navigationPostId);
    if (index < 0) {
        return false;
    }
    if (index == navigationLogicalIndex) {
        return true;
    }

    qCDebug(lcTimelineTrace).nospace()
        << "NAV_REMAP list=" << static_cast<const void*>(this)
        << " source=" << sourceName(postSource)
        << " postId=" << navigationPostId
        << " oldIndex=" << navigationLogicalIndex
        << " newIndex=" << index;

    if (!remapViewportLockedItem(index)) {
        return false;
    }
    navigationLogicalIndex = index;
    return true;
}

} // namespace Mattermost
