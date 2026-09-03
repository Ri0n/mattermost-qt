#include "PostsListWidget.h"

#include <algorithm>

#include <QPointer>
#include <QScrollBar>
#include <QThread>
#include <QTimer>

namespace Mattermost {

void PostsListWidget::addItem(QListWidgetItem* desiredItem)
{
    if (!desiredItem) {
        return;
    }

    if (timelineReconcileActive && isGapItem(desiredItem)) {
        reconcileTimelineGap(desiredItem);
        return;
    }

    QListWidget::addItem(desiredItem);
}

bool PostsListWidget::reconcileTimelineGap(QListWidgetItem* desiredGap)
{
    if (!timelineReconcileActive || !desiredGap || !isGapItem(desiredGap)) {
        return false;
    }

    QListWidgetItem* current = timelineReconcileCursor < count()
        ? item(timelineReconcileCursor) : nullptr;
    if (isGapItem(current)) {
        current->setData(ItemRole::gapFirstIndex,
                         desiredGap->data(ItemRole::gapFirstIndex));
        current->setData(ItemRole::gapCount,
                         desiredGap->data(ItemRole::gapCount));
        current->setFlags(desiredGap->flags());
        current->setSizeHint(desiredGap->sizeHint());
        delete desiredGap;
    } else {
        insertItem(timelineReconcileCursor, desiredGap);
    }

    ++timelineReconcileCursor;
    return true;
}

bool PostsListWidget::reconcileTimelinePost(PostWidget* postWidget)
{
    if (!timelineReconcileActive || !postWidget) {
        return false;
    }

    const QString postId = postWidget->post.id;
    int existingRow = -1;
    for (int row = timelineReconcileCursor; row < count(); ++row) {
        QListWidgetItem* candidate = item(row);
        if (isPostItem(candidate)
            && candidate->data(ItemRole::postId).toString() == postId) {
            existingRow = row;
            break;
        }
    }

    if (existingRow >= 0) {
        // Anything between the desired cursor and the already materialized post
        // is stale decoration or an evicted post. Removing preceding rows keeps
        // the existing QListWidgetItem/PostWidget identity intact, so persistent
        // model indexes used by asynchronous dimensionsChanged handlers remain
        // valid instead of being broken by takeItem()/reinsert moves.
        while (existingRow > timelineReconcileCursor) {
            removeTimelineRow(timelineReconcileCursor);
            --existingRow;
        }

        // Channel/Thread render code still holds this just-constructed pointer
        // for the remainder of the current call (it attaches one measurement
        // connection immediately after insertPost()). Defer deletion until the
        // event loop returns so reconciliation cannot create a use-after-free.
        postWidget->deleteLater();
        ++timelineReconcileCursor;
        return true;
    }

    // New materialization: reuse the ordinary insertion path so the row gets the
    // same resize/dimensionsChanged wiring as a live message. restoringSavedScroll
    // suppresses anchor churn until the reconciliation transaction commits.
    const bool reconcileWasActive = timelineReconcileActive;
    timelineReconcileActive = false;
    insertPost(timelineReconcileCursor, postWidget);
    timelineReconcileActive = reconcileWasActive;
    ++timelineReconcileCursor;
    return true;
}

bool PostsListWidget::reconcileTimelineDaySeparator(int daysAgo)
{
    if (!timelineReconcileActive) {
        return false;
    }

    QListWidgetItem* current = timelineReconcileCursor < count()
        ? item(timelineReconcileCursor) : nullptr;
    if (current
        && current->data(Qt::UserRole).toInt() == ItemType::separator
        && current->data(ItemRole::daySeparatorDays).isValid()) {
        if (current->data(ItemRole::daySeparatorDays).toInt() == daysAgo) {
            ++timelineReconcileCursor;
            return true;
        }
        removeTimelineRow(timelineReconcileCursor);
    }

    return false;
}

void PostsListWidget::removeTimelineRow(int rowIndex)
{
    if (rowIndex < 0 || rowIndex >= count()) {
        return;
    }

    QListWidgetItem* listItem = item(rowIndex);
    if (!listItem) {
        return;
    }

    if (listItem == newMessagesSeparator) {
        newMessagesSeparator = nullptr;
    }
    if (listItem == lastOwnPost) {
        lastOwnPost = nullptr;
    }
    if (listItem == currentEditedItem) {
        currentEditedItem = nullptr;
    }

    QWidget* rowWidget = itemWidget(listItem);
    if (rowWidget) {
        removeItemWidget(listItem);
        delete rowWidget;
    }

    delete takeItem(rowIndex);
}

void PostsListWidget::finishTimelineReconcile()
{
    if (!timelineReconcileActive) {
        return;
    }

    while (count() > timelineReconcileCursor) {
        removeTimelineRow(timelineReconcileCursor);
    }
    timelineReconcileActive = false;
}

void PostsListWidget::resumeTimelinePainting()
{
    if (!timelinePaintingSuspended) {
        return;
    }
    timelinePaintingSuspended = false;
    QWidget::setUpdatesEnabled(true);
    viewport()->update();
}

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

    // Keep the actual rows in place and reconcile the desired sparse sequence
    // against them. This is the key difference from the old implementation,
    // which called QListWidget::clear() for every REST page and therefore reset
    // the scrollbar range in the middle of an active thumb drag.
    timelineReconcileActive = true;
    timelineReconcileCursor = 0;

    // Painting is frozen only for this synchronous reconciliation call. It is
    // always re-enabled by the matching finishTimelineRebuild*() before control
    // returns to the event loop, avoiding the stale backing-store artefacts seen
    // when updates were previously disabled across queued callbacks.
    if (QWidget::updatesEnabled()) {
        QWidget::setUpdatesEnabled(false);
        timelinePaintingSuspended = true;
    }
}

void PostsListWidget::finishTimelineRebuildAtBottom()
{
    Q_ASSERT(QThread::currentThread() == thread());
    finishTimelineReconcile();

    if (!timelineNavigationPostId.isEmpty()) {
        // A pending semantic jump owns the viewport even before its row exists.
        // Never fall back to bottom while context is still being materialized.
        restoringSavedScroll = false;
        restoreTimelineNavigationLock();
        scheduleTimelineNavigationRestore();
        resumeTimelinePainting();
        return;
    }

    if (verticalScrollBar()->isSliderDown()) {
        restoringSavedScroll = false;
        if (savedScrollAnchor.valid) {
            savedScrollAnchor.atBottom = false;
        }
        resumeTimelinePainting();
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
    resumeTimelinePainting();
    QTimer::singleShot(0, this, apply);
}

bool PostsListWidget::finishTimelineRebuildAtPost(const QString& postId,
                                                   int viewportTopOffset)
{
    Q_ASSERT(QThread::currentThread() == thread());
    finishTimelineReconcile();

    if (!timelineNavigationPostId.isEmpty()) {
        restoringSavedScroll = false;
        const bool restored = restoreTimelineNavigationLock();
        scheduleTimelineNavigationRestore();
        resumeTimelinePainting();
        return restored || !timelineNavigationPostId.isEmpty();
    }

    const int rowIndex = findPostByIndex(postId, 0);
    if (rowIndex < 0) {
        restoringSavedScroll = false;
        resumeTimelinePainting();
        return false;
    }

    if (verticalScrollBar()->isSliderDown()) {
        restoringSavedScroll = false;
        if (savedScrollAnchor.valid) {
            savedScrollAnchor.atBottom = false;
        }
        resumeTimelinePainting();
        return true;
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
    resumeTimelinePainting();
    QTimer::singleShot(0, this, apply);
    return true;
}

void PostsListWidget::finishTimelineRebuildAtPixel(qint64 pixelOffset)
{
    Q_ASSERT(QThread::currentThread() == thread());
    finishTimelineReconcile();

    if (!timelineNavigationPostId.isEmpty()) {
        restoringSavedScroll = false;
        restoreTimelineNavigationLock();
        scheduleTimelineNavigationRestore();
        resumeTimelinePainting();
        return;
    }

    if (verticalScrollBar()->isSliderDown()) {
        restoringSavedScroll = false;
        if (savedScrollAnchor.valid) {
            savedScrollAnchor.atBottom = false;
        }
        resumeTimelinePainting();
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
    resumeTimelinePainting();
    QTimer::singleShot(0, this, apply);
}

} // namespace Mattermost
