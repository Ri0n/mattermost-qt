#include "ChatLogWidget.h"

#include <algorithm>

#include <QScrollBar>
#include <QTimer>
#include <QWheelEvent>

#include "ChatArea.h"
#include "backend/Backend.h"
#include "backend/types/BackendPost.h"
#include "post/PostWidget.h"

namespace Mattermost {

ChatLogWidget::ChatLogWidget(QWidget* parent)
    : LongListWidget(parent)
{
    setDefaultItemHeight(96);
    setMaterializationLimit(200);
    setRequestBlockSize(10);
    setPrefetchScreens(1);
    setSeekDebounceMs(100);

    navigationLockTimer.setSingleShot(true);
    connect(&navigationLockTimer, &QTimer::timeout,
            this, &ChatLogWidget::clearNavigationLock);

    connect(this, &LongListWidget::rangeRequested, this,
            [this](int first, int last, RequestReason reason, quint64 generation) {
        if (!postSource) {
            finishRangeRequest(first, last);
            return;
        }
        postSource->requestRange(first, last, toSourceReason(reason), generation);
    });

    connect(this, &LongListWidget::visibleRangeChanged, this,
            [this](int first, int) {
        if (!postSource || first < 0 || first > 1 || !postSource->canRequestBeforeFirst()) {
            return;
        }
        postSource->requestBeforeFirst(AbstractPostSource::RequestReason::Scroll, 0);
    });

    // QScrollBar::setValue() used by LongListWidget's geometry transactions does
    // not emit actionTriggered/sliderReleased, so these are safe user-gesture
    // notifications. Defer actionTriggered because Qt updates the value as part
    // of the same slider action.
    connect(verticalScrollBar(), &QScrollBar::actionTriggered, this, [this](int) {
        QTimer::singleShot(0, this, [this] {
            emit userViewportChanged(isAtEnd());
        });
    });
    connect(verticalScrollBar(), &QScrollBar::sliderReleased, this, [this] {
        emit userViewportChanged(isAtEnd());
    });

    // Semantic navigation is intentionally canceled only by real user scroll
    // gestures. Geometry corrections and source-driven remapping never emit
    // userViewportChanged(), so they keep the post-ID lock intact.
    connect(this, &LongListWidget::userViewportChanged, this,
            [this](bool) { clearNavigationLock(); });
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
    navigationAlignment = alignment;
    navigationLogicalIndex = -1;
    navigationQuietPeriodMs = std::max(0, quietPeriodMs);
    restoreNavigationTarget(true);
    return true;
}

void ChatLogWidget::clearNavigationLock()
{
    navigationLockTimer.stop();
    navigationPostId.clear();
    navigationLogicalIndex = -1;
    navigationQuietPeriodMs = 0;
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
    connect(widget, &PostWidget::dimensionsChanged, this, [this, postId] {
        if (!postSource) {
            return;
        }
        const int currentIndex = postSource->indexOfPost(postId);
        if (currentIndex >= 0) {
            itemsChanged(currentIndex, currentIndex);
        }
    });
    return widget;
}

void ChatLogWidget::destroyItemWidget(int index, QWidget* widget)
{
    if (editedPostWidget == widget) {
        editedPostWidget.clear();
    }
    LongListWidget::destroyItemWidget(index, widget);
}

void ChatLogWidget::wheelEvent(QWheelEvent* event)
{
    LongListWidget::wheelEvent(event);
    emit userViewportChanged(isAtEnd());
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
        setItemCount(count);
        restoreNavigationTarget();
    }));
    sourceConnections.push_back(connect(postSource, &AbstractPostSource::itemsInserted,
                                        this, [this](int first, int count) {
        insertItems(first, count);
        restoreNavigationTarget();
    }));
    sourceConnections.push_back(connect(postSource, &AbstractPostSource::rangeAvailable,
                                        this, [this](int first, int last) {
        setRangeAvailable(first, last, true);
        restoreNavigationTarget();
    }));
    sourceConnections.push_back(connect(postSource, &AbstractPostSource::itemsChanged,
                                        this, [this](int first, int last) {
        rematerializeRange(first, last);
        restoreNavigationTarget();
    }));
    sourceConnections.push_back(connect(postSource, &AbstractPostSource::rangeRequestFinished,
                                        this, [this](int first, int last) {
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

    for (int index = first; index <= last; ++index) {
        const bool sourceAvailable = postSource->isAvailable(index);
        if (itemWidget(index)) {
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

bool ChatLogWidget::restoreNavigationTarget(bool force)
{
    if (!postSource || navigationPostId.isEmpty()) {
        return false;
    }

    const int index = postSource->indexOfPost(navigationPostId);
    if (index < 0) {
        return false;
    }
    if (!force && index == navigationLogicalIndex) {
        return true;
    }

    navigationLogicalIndex = index;
    scrollToIndex(index, navigationAlignment);
    touchNavigationLock();
    return true;
}

void ChatLogWidget::touchNavigationLock()
{
    if (navigationPostId.isEmpty() || navigationQuietPeriodMs <= 0) {
        return;
    }
    navigationLockTimer.start(navigationQuietPeriodMs);
}

} // namespace Mattermost
