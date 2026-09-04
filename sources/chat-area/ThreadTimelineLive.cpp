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
#include <QPointer>
#include <QScrollBar>
#include <QTimer>

#include "ChatArea.h"
#include "PostsListWidget.h"
#include "ThreadTimelinePolicy.h"
#include "backend/PostTimelineService.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "post/PostWidget.h"
#include "ui_ChatArea.h"

namespace Mattermost {
namespace {

constexpr int InitialThreadWindowSize = 30;

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
    // the full-render path. One more event-loop turn gives explicit
    // permalink/Attention/Recent navigation a chance to install its semantic
    // target before the ordinary "open at newest" policy runs.
    QTimer::singleShot(0, controller, [controller] {
        controller->installIncrementalLiveUpdates();
        QTimer::singleShot(0, controller, [controller] {
            controller->openNewestOnInitialOpen();
        });
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

void ThreadTimelineController::openNewestOnInitialOpen()
{
    if (initialNewestOpenDone || initialNewestRequestInFlight
        || !area.ui || !area.ui->listWidget || rootId.isEmpty()) {
        return;
    }

    PostsListWidget* list = area.ui->listWidget;

    // Explicit navigation owns the viewport. Attention/Recent/permalink paths
    // have already selected an exact reply (or last_viewed_at-derived reply), so
    // the ordinary thread-opening policy must not override it with the newest row.
    if (list->hasTimelineNavigationLock()) {
        initialNewestOpenDone = true;
        return;
    }

    BackendPost* rootPost = area.channel.postIdToPost.value(rootId, nullptr);
    expectedPostCount = std::max(expectedPostCount,
                                 expectedThreadPostCount(rootPost, expectedPostCount));

    // A compact thread is already completely covered by the normal first page.
    // Wait for that one request rather than issuing a redundant tail request.
    if (threadInitialPageContainsNewest(expectedPostCount, InitialThreadWindowSize)) {
        if (!initialPrefetchDone) {
            if (requestInFlight) {
                QPointer<ThreadTimelineController> guard(this);
                QTimer::singleShot(25, this, [guard] {
                    if (guard) {
                        guard->openNewestOnInitialOpen();
                    }
                });
                return;
            }
            // The initial request failed or there is only the root. Whatever is
            // currently materialized is the best available newest edge.
        }

        if (!list->hasTimelineNavigationLock()) {
            list->scrollToBottom();
        }
        initialNewestOpenDone = true;
        return;
    }

    if (!rootPost || rootPost->last_reply_at == 0) {
        // Metadata may arrive slightly after the root itself. Do not manufacture
        // an approximate timestamp; retry once the normal first page has had a
        // chance to refresh the root metadata.
        if (requestInFlight) {
            QPointer<ThreadTimelineController> guard(this);
            QTimer::singleShot(25, this, [guard] {
                if (guard) {
                    guard->openNewestOnInitialOpen();
                }
            });
        }
        return;
    }

    initialNewestRequestInFlight = true;
    QPointer<ThreadTimelineController> guard(this);
    PostTimelineService::instance(area.backend).loadThreadTail(
        area.channel, rootId, InitialThreadWindowSize, rootPost->last_reply_at,
        [guard](const PostTimelineService::Page& page) {
            if (!guard) {
                return;
            }

            guard->initialNewestRequestInFlight = false;
            if (!page.success || !guard->area.ui || !guard->area.ui->listWidget) {
                return;
            }

            PostsListWidget* currentList = guard->area.ui->listWidget;
            if (currentList->hasTimelineNavigationLock()) {
                // The response is still useful in BackendChannel's cache, but an
                // explicit target that appeared while the request was in flight
                // remains authoritative for the visible sparse timeline.
                guard->initialNewestOpenDone = true;
                return;
            }

            QStringList tailIds = page.postIds;
            tailIds.removeAll(guard->rootId);
            if (tailIds.isEmpty()) {
                currentList->scrollToBottom();
                guard->initialNewestOpenDone = true;
                return;
            }

            BackendPost* currentRoot = guard->area.channel.postIdToPost.value(
                guard->rootId, nullptr);
            guard->expectedPostCount = std::max(
                guard->expectedPostCount,
                expectedThreadPostCount(currentRoot, guard->expectedPostCount));
            guard->timeline.setTotalCount(guard->expectedPostCount);

            const int firstIndex = threadTailWindowFirstIndex(
                guard->expectedPostCount, static_cast<int>(tailIds.size()));
            guard->timeline.placeWindow(firstIndex, tailIds);

            ViewportAnchor bottomAnchor;
            bottomAnchor.kind = ViewportAnchor::Bottom;
            guard->renderTimeline(QString(), false, bottomAnchor);
            guard->scheduleMeasurementPass();
            guard->schedulePrune();
            guard->persistState();
            guard->initialNewestOpenDone = true;
        });
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
