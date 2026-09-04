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

    // start() owns initial positioning. This queued hook only replaces the
    // legacy live-post handler; keeping a second independent initial-open timer
    // would reintroduce competing owners of the thread viewport.
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

    // Scroll/seek wiring is owned by ThreadTimelineController::start() and is
    // shared with the channel controller's seek semantics. Do not disconnect or
    // replace it here: this helper is only responsible for the live-post path.
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
    timeline.setTotalCount(expectedPostCount);
    if (rootPost && !timeline.contains(rootId)) {
        timeline.placeWindow(0, QStringList {rootId});
    }

    // Root-only thread: there is no sparse tail to materialize. Render the root
    // and establish bottom only after that concrete row exists.
    if (expectedPostCount <= 1) {
        ViewportAnchor bottomAnchor;
        bottomAnchor.kind = ViewportAnchor::Bottom;
        renderTimeline(QString(), false, bottomAnchor);
        scheduleMeasurementPass();
        persistState();
        initialNewestOpenDone = true;
        return;
    }

    // The previous implementation treated a <=30-reply thread as if start()
    // had already fetched its first page. start() is now intentionally tail-first,
    // so that assumption left only root+gap materialized and scrollToBottom()
    // landed on an empty gap. Every non-empty ordinary thread must fetch the
    // newest window first, compact or large, before bottom becomes visible.
    if (!rootPost || rootPost->last_reply_at == 0) {
        // Channel root posts normally carry reply_count/last_reply_at. If a
        // server omitted the timestamp, do not fake a bottom position over an
        // unmaterialized gap. A later root metadata update/navigation can retry.
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

            BackendPost* currentRoot = guard->area.channel.postIdToPost.value(
                guard->rootId, nullptr);
            guard->expectedPostCount = std::max(
                guard->expectedPostCount,
                expectedThreadPostCount(currentRoot, guard->expectedPostCount));
            guard->timeline.setTotalCount(guard->expectedPostCount);
            if (currentRoot && !guard->timeline.contains(guard->rootId)) {
                guard->timeline.placeWindow(0, QStringList {guard->rootId});
            }

            if (!tailIds.isEmpty()) {
                const int firstIndex = threadTailWindowFirstIndex(
                    guard->expectedPostCount, static_cast<int>(tailIds.size()));
                guard->timeline.placeWindow(firstIndex, tailIds);
                // The response is explicitly the newest server window. Align it
                // to the newest logical boundary even if reply_count metadata was
                // briefly stale; bottom must end on a PostWidget, never a gap.
                guard->timeline.alignLoadedSpanToBoundary(tailIds.last(), false);
            }

            ViewportAnchor bottomAnchor;
            bottomAnchor.kind = ViewportAnchor::Bottom;
            guard->renderTimeline(QString(), false, bottomAnchor);
            guard->scheduleMeasurementPass();
            guard->persistState();
            guard->initialNewestOpenDone = true;
            guard->schedulePrune();
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
    // here. The common seek wiring ignores layout-driven scrollbar changes.
    scheduleMeasurementPass();
    schedulePrune();
    persistState();
}

} // namespace Mattermost
