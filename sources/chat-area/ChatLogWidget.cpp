#include "ChatLogWidget.h"

#include <algorithm>

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

    connect(this, &LongListWidget::rangeRequested, this,
            [this](int first, int last, RequestReason reason, quint64 generation) {
        if (!postSource) {
            finishRangeRequest(first, last);
            return;
        }
        postSource->requestRange(first, last, toSourceReason(reason), generation);
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
    const int index = postSource->indexOfPost(postId);
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

    auto* widget = new PostWidget(*backend, *post, viewport(), chatArea, lastRootPost);
    connect(widget, &PostWidget::dimensionsChanged, this, [this, index] {
        itemsChanged(index, index);
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
    }));
    sourceConnections.push_back(connect(postSource, &AbstractPostSource::rangeAvailable,
                                        this, [this](int first, int last) {
        setRangeAvailable(first, last, true);
    }));
    sourceConnections.push_back(connect(postSource, &AbstractPostSource::itemsChanged,
                                        this, [this](int first, int last) {
        rematerializeRange(first, last);
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
        if (!itemWidget(index)) {
            continue;
        }
        setRangeAvailable(index, index, false);
        if (postSource->isAvailable(index)) {
            setRangeAvailable(index, index, true);
        }
    }
}

} // namespace Mattermost
