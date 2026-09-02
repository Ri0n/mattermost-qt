#include "PostsListWidget.h"

#include <algorithm>

#include <QScrollBar>
#include <QThread>
#include <QTimer>

namespace Mattermost {

void PostsListWidget::lockTimelineNavigationToPost(const QString& postId,
                                                   int viewportTopOffset,
                                                   int quietPeriodMs)
{
    Q_ASSERT(QThread::currentThread() == thread());

    if (postId.isEmpty()) {
        clearTimelineNavigationLock();
        return;
    }

    if (!timelineNavigationUserCancelConnected) {
        timelineNavigationUserCancelConnected = true;
        connect(this, &PostsListWidget::userViewportChanged, this,
                [this](bool) { clearTimelineNavigationLock(); });
    }

    if (!timelineNavigationGeometryConnected) {
        timelineNavigationGeometryConnected = true;

        QScrollBar* bar = verticalScrollBar();
        connect(bar, &QScrollBar::rangeChanged, this,
                [this](int, int) { scheduleTimelineNavigationRestore(); });
        connect(bar, &QScrollBar::valueChanged, this,
                [this](int) {
            if (!restoringSavedScroll && !isUserScrollInProgress()) {
                scheduleTimelineNavigationRestore();
            }
        });
    }

    // Semantic navigation always detaches from the live bottom. Even before the
    // target row has been materialized, no automatic population/layout path may
    // reinterpret the temporary scrollbar position as "follow latest".
    if (savedScrollAnchor.valid) {
        savedScrollAnchor.atBottom = false;
    }

    timelineNavigationPostId = postId;
    timelineNavigationTopOffset = std::max(0, viewportTopOffset);
    timelineNavigationQuietPeriodMs = std::max(0, quietPeriodMs);
    touchTimelineNavigationLock();
    scheduleTimelineNavigationRestore();
}

void PostsListWidget::clearTimelineNavigationLock()
{
    Q_ASSERT(QThread::currentThread() == thread());
    ++timelineNavigationLockGeneration;
    timelineNavigationPostId.clear();
    timelineNavigationRestoreScheduled = false;
}

void PostsListWidget::touchTimelineNavigationLock()
{
    if (timelineNavigationPostId.isEmpty()) {
        return;
    }

    const quint64 generation = ++timelineNavigationLockGeneration;
    const int quietPeriodMs = timelineNavigationQuietPeriodMs;
    if (quietPeriodMs <= 0) {
        return;
    }

    QTimer::singleShot(quietPeriodMs, this, [this, generation] {
        if (generation == timelineNavigationLockGeneration) {
            timelineNavigationPostId.clear();
        }
    });
}

void PostsListWidget::scheduleTimelineNavigationRestore()
{
    if (timelineNavigationPostId.isEmpty()
        || timelineNavigationRestoreScheduled
        || restoringSavedScroll
        || isUserScrollInProgress()) {
        return;
    }

    timelineNavigationRestoreScheduled = true;
    QTimer::singleShot(0, this, [this] {
        timelineNavigationRestoreScheduled = false;
        if (timelineNavigationPostId.isEmpty()
            || restoringSavedScroll
            || isUserScrollInProgress()) {
            return;
        }
        restoreTimelineNavigationLock();
    });
}

bool PostsListWidget::restoreTimelineNavigationLock()
{
    Q_ASSERT(QThread::currentThread() == thread());

    if (timelineNavigationPostId.isEmpty()) {
        return false;
    }

    const QString postId = timelineNavigationPostId;
    const int rowIndex = findPostByIndex(postId, 0);
    if (rowIndex < 0) {
        return false;
    }

    const quint64 generation = ++scrollIntentGeneration;
    const int viewportTopOffset = timelineNavigationTopOffset;
    auto apply = [this, generation, postId, viewportTopOffset] {
        if (generation != scrollIntentGeneration) {
            return;
        }

        const int currentRow = findPostByIndex(postId, 0);
        if (currentRow < 0) {
            return;
        }
        QListWidgetItem* anchorItem = item(currentRow);
        if (!anchorItem) {
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
        if (savedScrollAnchor.valid) {
            savedScrollAnchor.atBottom = false;
        }
    };

    apply();
    QTimer::singleShot(0, this, apply);
    touchTimelineNavigationLock();
    return true;
}

void PostsListWidget::beginTimelineRebuild()
{
    Q_ASSERT(QThread::currentThread() == thread());

    ++scrollIntentGeneration;
    restoringSavedScroll = true;

    removeNewMessagesSeparatorTimer.stop();
    savedScrollRestoreScheduled = false;
    userScrollAnchorUpdateScheduled = false;
    pendingScrollBarUserIntent = false;
    handlingUserScrollEvent = false;

    // A sparse-timeline rebuild owns viewport restoration as one transaction.
    // Do not let the ordinary clear()/insertPost() path capture and repeatedly
    // restore intermediate geometries while rows are replaced. A semantic
    // navigation lock is deliberately kept separately and remains authoritative
    // even while its target row is temporarily absent.
    savedScrollAnchor = SavedScrollAnchor();
    QListWidget::clear();
    newMessagesSeparator = nullptr;
    lastOwnPost = nullptr;
    currentEditedItem = nullptr;
}

void PostsListWidget::finishTimelineRebuildAtBottom()
{
    Q_ASSERT(QThread::currentThread() == thread());

    if (!timelineNavigationPostId.isEmpty()) {
        // A pending semantic jump owns the viewport even before its row exists.
        // Never fall back to bottom while context is still being materialized.
        restoringSavedScroll = false;
        restoreTimelineNavigationLock();
        scheduleTimelineNavigationRestore();
        return;
    }

    const quint64 generation = ++scrollIntentGeneration;

    auto apply = [this, generation] {
        if (generation != scrollIntentGeneration) {
            return;
        }
        restoringSavedScroll = true;
        QListWidget::scrollToBottom();
        restoringSavedScroll = false;
        saveScrollAnchor();
        if (savedScrollAnchor.valid) {
            savedScrollAnchor.atBottom = true;
        }
    };

    apply();
    QTimer::singleShot(0, this, apply);
}

bool PostsListWidget::finishTimelineRebuildAtPost(const QString& postId,
                                                   int viewportTopOffset)
{
    Q_ASSERT(QThread::currentThread() == thread());

    if (!timelineNavigationPostId.isEmpty()) {
        restoringSavedScroll = false;
        restoreTimelineNavigationLock();
        scheduleTimelineNavigationRestore();
        return true;
    }

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
        if (savedScrollAnchor.valid) {
            savedScrollAnchor.atBottom = false;
        }
    };

    apply();
    QTimer::singleShot(0, this, apply);
    return true;
}

void PostsListWidget::finishTimelineRebuildAtPixel(qint64 pixelOffset)
{
    Q_ASSERT(QThread::currentThread() == thread());

    if (!timelineNavigationPostId.isEmpty()) {
        restoringSavedScroll = false;
        restoreTimelineNavigationLock();
        scheduleTimelineNavigationRestore();
        return;
    }

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
        if (savedScrollAnchor.valid) {
            savedScrollAnchor.atBottom = false;
        }
    };

    apply();
    QTimer::singleShot(0, this, apply);
}

} // namespace Mattermost
