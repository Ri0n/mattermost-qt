#include "PostsListWidget.h"

#include <algorithm>

#include <QScrollBar>
#include <QTimer>

namespace Mattermost {

void PostsListWidget::beginTimelineRebuild()
{
    ++scrollIntentGeneration;
    restoringSavedScroll = true;

    removeNewMessagesSeparatorTimer.stop();
    savedScrollRestoreScheduled = false;
    userScrollAnchorUpdateScheduled = false;
    pendingScrollBarUserIntent = false;
    handlingUserScrollEvent = false;

    // A sparse-timeline rebuild owns viewport restoration as one transaction.
    // Do not let the ordinary clear()/insertPost() path capture and repeatedly
    // restore intermediate geometries while hundreds of rows are replaced.
    savedScrollAnchor = SavedScrollAnchor();
    QListWidget::clear();
    newMessagesSeparator = nullptr;
    lastOwnPost = nullptr;
    currentEditedItem = nullptr;
}

void PostsListWidget::finishTimelineRebuildAtBottom()
{
    const quint64 generation = ++scrollIntentGeneration;

    auto apply = [this, generation] {
        if (generation != scrollIntentGeneration) {
            return;
        }
        restoringSavedScroll = true;
        QListWidget::scrollToBottom();
        restoringSavedScroll = false;
        saveScrollAnchor();
    };

    apply();
    QTimer::singleShot(0, this, apply);
}

bool PostsListWidget::finishTimelineRebuildAtPost(const QString& postId,
                                                   int viewportTopOffset)
{
    const int rowIndex = findPostByIndex(postId, 0);
    if (rowIndex < 0) {
        restoringSavedScroll = false;
        return false;
    }

    const quint64 generation = ++scrollIntentGeneration;
    auto apply = [this, generation, postId, viewportTopOffset] {
        if (generation != scrollIntentGeneration) {
            return;
        }

        const int currentRow = findPostByIndex(postId, 0);
        if (currentRow < 0) {
            restoringSavedScroll = false;
            return;
        }
        QListWidgetItem* anchorItem = item(currentRow);
        if (!anchorItem) {
            restoringSavedScroll = false;
            return;
        }

        restoringSavedScroll = true;
        QListWidget::scrollToItem(anchorItem, QAbstractItemView::PositionAtTop);

        const QRect rect = visualItemRect(anchorItem);
        if (rect.isValid()) {
            const int delta = rect.top() - viewportTopOffset;
            if (delta != 0) {
                QScrollBar* bar = verticalScrollBar();
                bar->setValue(bar->value() + delta);
            }
        }
        restoringSavedScroll = false;
        saveScrollAnchor();
    };

    apply();
    QTimer::singleShot(0, this, apply);
    return true;
}

void PostsListWidget::finishTimelineRebuildAtPixel(qint64 pixelOffset)
{
    const quint64 generation = ++scrollIntentGeneration;

    auto apply = [this, generation, pixelOffset] {
        if (generation != scrollIntentGeneration) {
            return;
        }
        QScrollBar* bar = verticalScrollBar();
        const qint64 bounded = std::max<qint64>(0,
            std::min<qint64>(pixelOffset, bar->maximum()));
        restoringSavedScroll = true;
        bar->setValue(static_cast<int>(bounded));
        restoringSavedScroll = false;
        saveScrollAnchor();
    };

    apply();
    QTimer::singleShot(0, this, apply);
}

} // namespace Mattermost
