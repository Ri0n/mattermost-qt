/**
 * @file ThreadTimelineLive.cpp
 * @brief Incremental live-reply materialization for sparse thread timelines.
 *
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Mattermost-QT is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Mattermost-QT. if not, see https://www.gnu.org/licenses/.
 */

#include "ThreadTimelineController.h"

#include <algorithm>
#include <climits>

#include <QDateTime>
#include <QListWidgetItem>
#include <QScrollBar>
#include <QTimer>

#include "ChatArea.h"
#include "PostsListWidget.h"
#include "ThreadTimelinePolicy.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "post/PostWidget.h"
#include "ui_ChatArea.h"

namespace Mattermost {
namespace {

int expectedThreadPostCount(const BackendPost* root, int fallback)
{
    if (!root) {
        return std::max(1, fallback);
    }

    const int64_t boundedReplies = std::min<int64_t>(root->reply_count, INT_MAX - 1);
    return std::max(1, static_cast<int>(boundedReplies) + 1);
}

void applyGapSpan(QListWidgetItem& item, const PostTimeline::Span& span)
{
    item.setData(Qt::UserRole, ItemType::gap);
    item.setData(ItemRole::gapFirstIndex, span.firstIndex);
    item.setData(ItemRole::gapCount, span.count);
    item.setFlags(Qt::NoItemFlags);
    const qint64 boundedHeight = std::min<qint64>(span.estimatedHeight, INT_MAX);
    item.setSizeHint(QSize(0, static_cast<int>(boundedHeight)));
}

void syncGapBeforeLivePost(PostsListWidget& list,
                           const PostTimeline& timeline,
                           int liveLogicalIndex)
{
    const PostTimeline::Span* gapBeforeLive = nullptr;
    const QVector<PostTimeline::Span> spans = timeline.spans();
    for (const PostTimeline::Span& span : spans) {
        if (span.kind == PostTimeline::GapSpan
            && span.firstIndex + span.count == liveLogicalIndex) {
            gapBeforeLive = &span;
            break;
        }
    }

    QListWidgetItem* lastItem = list.count() > 0 ? list.item(list.count() - 1) : nullptr;
    if (!gapBeforeLive) {
        if (PostsListWidget::isGapItem(lastItem)) {
            delete list.takeItem(list.count() - 1);
        }
        return;
    }

    if (PostsListWidget::isGapItem(lastItem)) {
        applyGapSpan(*lastItem, *gapBeforeLive);
        return;
    }

    auto* gapItem = new QListWidgetItem;
    applyGapSpan(*gapItem, *gapBeforeLive);
    list.addItem(gapItem);
}

} // namespace

ThreadTimelineController* createConfiguredThreadTimelineController(ChatArea& area)
{
    ThreadTimelineController* controller = createThreadTimelineController(area);
    if (!controller) {
        return nullptr;
    }

    // start() was queued by the controller constructor before this callback is
    // queued. Replace its legacy live handler in the same event-loop turn,
    // rather than leaving a second-turn window in which a reply can still take
    // the full-render path.
    QTimer::singleShot(0, controller, [controller] {
        controller->installIncrementalLiveUpdates();
    });
    return controller;
}

void ThreadTimelineController::installIncrementalLiveUpdates()
{
    QObject::disconnect(&area.channel, &BackendChannel::onNewPost, this, nullptr);
    connect(&area.channel, &BackendChannel::onNewPost,
            this, &ThreadTimelineController::materializeLivePost,
            Qt::UniqueConnection);

    // Raw scrollbar valueChanged also fires for item insertion, gap resizing,
    // Markdown/image reflow and anchor restoration. Those are layout changes,
    // not navigation, and must never trigger a REST seek/page request. Wheel and
    // keyboard movement already arrive through userViewportChanged; thumb drags
    // are handled here only while the slider is physically down.
    if (area.ui && area.ui->listWidget) {
        QScrollBar* scrollBar = area.ui->listWidget->verticalScrollBar();
        QObject::disconnect(scrollBar, &QScrollBar::valueChanged, this, nullptr);
        connect(scrollBar, &QScrollBar::valueChanged, this,
                [this, scrollBar](int) {
            if (scrollBar->isSliderDown()) {
                scheduleViewportCheck();
            }
        });
    }
}

void ThreadTimelineController::materializeLivePost(BackendPost& post)
{
    if (post.root_id != rootId) {
        return;
    }

    const bool alreadyMaterialized = timeline.contains(post.id);
    BackendPost* rootPost = area.channel.postIdToPost.value(rootId, nullptr);
    const int oldLogicalCount = std::max(1, timeline.totalCount());
    const int reportedExpected = expectedThreadPostCount(rootPost, expectedPostCount);

    expectedPostCount = threadExpectedCountAfterLiveReply(
        expectedPostCount, reportedExpected, alreadyMaterialized);
    timeline.setTotalCount(expectedPostCount);

    if (alreadyMaterialized) {
        scheduleMeasurementPass();
        schedulePrune();
        return;
    }

    const int liveLogicalIndex = std::max(1, expectedPostCount - 1);
    timeline.placeWindow(liveLogicalIndex, QStringList {post.id});

    // If sequential cursor paging had already reached the old newest edge, the
    // websocket reply extends that known prefix by exactly one row. Keep cursor
    // state caught up instead of making a later viewport check refetch this post.
    if (nextLogicalIndex >= oldLogicalCount) {
        nextLogicalIndex = expectedPostCount;
        if (!hasNext) {
            cursorPostId = post.id;
            cursorCreateAt = post.create_at;
        }
    }

    if (!initialPrefetchDone || !area.ui || !area.ui->listWidget) {
        scheduleMeasurementPass();
        schedulePrune();
        persistState();
        return;
    }

    PostsListWidget& list = *area.ui->listWidget;
    syncGapBeforeLivePost(list, timeline, liveLogicalIndex);

    BackendPost* previousPost = nullptr;
    BackendPost* lastRootPost = nullptr;
    if (liveLogicalIndex > 0) {
        const QString previousId = timeline.postIdAt(liveLogicalIndex - 1);
        if (!previousId.isEmpty()) {
            previousPost = area.channel.postIdToPost.value(previousId, nullptr);
            if (previousPost) {
                lastRootPost = previousPost->rootPost;
            }
        }
    }

    // A date separator is authoritative only when the immediately preceding
    // logical row is materialized. A sparse gap may hide arbitrary day changes.
    if (previousPost
        && previousPost->getCreationTime().date() != post.getCreationTime().date()) {
        const int daysAgo = post.getCreationTime().date()
            .daysTo(QDateTime::currentDateTime().date());
        list.addDaySeparator(daysAgo);
    }

    auto* postWidget = new PostWidget(area.backend, post, &list, &area, lastRootPost);
    list.insertPost(postWidget);
    connect(postWidget, &PostWidget::dimensionsChanged,
            this, &ThreadTimelineController::scheduleMeasurementPass);

    // One websocket reply is deliberately a one-row transaction. No
    // renderTimeline(), no scrollToBottom(), and no viewport check is scheduled
    // here. User scrolling will request neighbouring gaps when needed; layout
    // driven scrollbar changes are ignored by installIncrementalLiveUpdates().
    scheduleMeasurementPass();
    schedulePrune();
    persistState();
}

} // namespace Mattermost
