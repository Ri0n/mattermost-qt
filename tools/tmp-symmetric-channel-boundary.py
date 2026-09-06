from pathlib import Path


def replace_function(text: str, signature: str, replacement: str) -> str:
    start = text.index(signature)
    brace = text.index('{', start)
    depth = 0
    end = None
    for i in range(brace, len(text)):
        ch = text[i]
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
            if depth == 0:
                end = i + 1
                break
    if end is None:
        raise RuntimeError(f'unterminated function: {signature}')
    return text[:start] + replacement.rstrip() + text[end:]


cpp_path = Path('sources/chat-area/ChannelPostSource.cpp')
h_path = Path('sources/chat-area/ChannelPostSource.h')
long_doc_path = Path('docs/long-list-architecture.md')
cache_doc_path = Path('docs/post-cache.md')
thread_doc_path = Path('docs/thread-timeline-loading.md')

cpp = cpp_path.read_text()
cpp = cpp.replace('exactRootCount', 'hasRootCountEstimate')
cpp = cpp.replace('rootCountOverestimate', 'rootCountAdjustment')
cpp = cpp.replace('++rootCountAdjustment;', '--rootCountAdjustment;')

cpp = replace_function(cpp, 'void ChannelPostSource::requestRange(', r'''void ChannelPostSource::requestRange(int first,
                                     int last,
                                     RequestReason reason,
                                     quint64 generation)
{
    Q_UNUSED(reason)
    Q_UNUSED(generation)

    if (!hasRootCountEstimate || postIds.isEmpty()) {
        emit rangeRequestFinished(first, last);
        return;
    }

    const int requestedFirst = std::max(0, first);
    const int requestedLast = std::min(static_cast<int>(postIds.size()) - 1, last);
    if (requestedLast < requestedFirst) {
        emit rangeRequestFinished(first, last);
        return;
    }

    // total_msg_count_root is only an initial coordinate estimate. Deleted
    // roots can make it too large, while join/leave and other count-excluded
    // system roots returned by /posts can make it too small. Large top-edge
    // seeks start 3% inside that estimate and repair the oldest boundary in
    // either direction before ordinary page placement resumes.
    if (requestedFirst == 0 && !oldestBoundaryFastPathTried
        && initialBoundaryProbePages() > 1) {
        oldestBoundaryFastPathTried = true;
        QPointer<ChannelPostSource> guard(this);
        probeEstimatedOldestBoundary([guard, first, last] {
            if (guard) {
                emit guard->rangeRequestFinished(first, last);
            }
        });
        return;
    }

    int firstMissing = requestedFirst;
    while (firstMissing <= requestedLast && isAvailable(firstMissing)) {
        ++firstMissing;
    }
    if (firstMissing > requestedLast) {
        emit rangeRequestFinished(first, last);
        return;
    }

    int lastMissing = requestedLast;
    while (lastMissing > firstMissing && isAvailable(lastMissing)) {
        --lastMissing;
    }

    // Mattermost channel history has one paging unit here: ten root posts.
    // Logical list blocks are anchored at the oldest end while Mattermost page
    // numbers are anchored at the newest end, so one ten-item logical request
    // can straddle two server pages. Load every absolute page intersecting the
    // missing range; do not turn already known post identities into paging
    // cursors. Those identities are useful for semantic-position estimation and
    // overlap reconciliation, not for choosing the next HTTP request boundary.
    const int newestPage = pageForIndex(lastMissing);
    const int oldestPage = pageForIndex(firstMissing);
    const int pageCount = oldestPage - newestPage + 1;
    if (pageCount <= 0) {
        emit rangeRequestFinished(first, last);
        return;
    }

    QPointer<ChannelPostSource> guard(this);
    auto pending = std::make_shared<int>(pageCount);
    const auto finishPage = [guard, pending, first, last] {
        if (!guard) {
            return;
        }
        if (--(*pending) == 0) {
            emit guard->rangeRequestFinished(first, last);
        }
    };

    for (int page = newestPage; page <= oldestPage; ++page) {
        qCDebug(lcTimelineChannel).nospace()
            << "RANGE_PAGE requested=[" << requestedFirst << ',' << requestedLast
            << "] missing=[" << firstMissing << ',' << lastMissing
            << "] page=" << page << " perPage=" << ServerPageSize;
        PostTimelineService::instance(backend).loadChannelPage(
            channel, page, ServerPageSize,
            [guard, page, requestedFirst, oldestPage, finishPage](
                const PostTimelineService::Page& result) {
                if (!guard) {
                    return;
                }
                if (!result.success) {
                    finishPage();
                    return;
                }
                if (result.postIds.isEmpty()) {
                    guard->resolveOldestBoundary(page, finishPage);
                    return;
                }

                guard->placePage(page, result.postIds);

                // For small estimates we deliberately take the normal ten-post
                // path first. If the estimated oldest page is full, however,
                // the count may be an underestimate (for example because
                // join/leave system posts are excluded from the counter). Keep
                // this request open and search farther into older pages.
                if (requestedFirst == 0 && page == oldestPage
                    && result.postIds.size() == ServerPageSize
                    && !guard->oldestBoundaryFastPathTried) {
                    guard->resolveOldestBoundaryFromNonEmpty(page, finishPage);
                    return;
                }
                finishPage();
            });
    }
}''')

cpp = replace_function(cpp, 'void ChannelPostSource::probeEstimatedOldestBoundary(', r'''void ChannelPostSource::probeEstimatedOldestBoundary(
    std::function<void()> completion)
{
    if (completion) {
        oldestBoundaryWaiters.push_back(std::move(completion));
    }
    if (oldestBoundaryProbeInFlight || postIds.isEmpty()) {
        return;
    }

    oldestBoundaryProbeInFlight = true;
    oldestBoundaryNonEmptyPage = -1;
    oldestBoundaryEmptyPage = -1;
    oldestBoundaryProbeStep = initialBoundaryProbePages();
    oldestBoundarySearchLimitPage = pageForIndex(0);
    oldestBoundaryMaterializedFullPage = -1;

    // Start at the expected deletion distance instead of spending a round trip
    // on the estimated oldest page. Empty means the estimate was too large and
    // we step inward. Existing data is searched outward to the estimate; if the
    // estimated page is full, search continues beyond it because count-excluded
    // system roots can make total_msg_count_root an underestimate as well.
    const int page = std::max(0,
                              oldestBoundarySearchLimitPage - oldestBoundaryProbeStep);
    const int offset = page * ServerPageSize;
    qCDebug(lcTimelineChannel).nospace()
        << "OLDEST_BOUNDARY_INITIAL page=" << page
        << " offset=" << offset
        << " limit=" << oldestBoundarySearchLimitPage
        << " step=" << oldestBoundaryProbeStep
        << " perPage=1";

    QPointer<ChannelPostSource> guard(this);
    PostTimelineService::instance(backend).loadChannelPage(
        channel, offset, 1,
        [guard, page, offset](const PostTimelineService::Page& result) {
            if (!guard || !guard->oldestBoundaryProbeInFlight) {
                return;
            }
            if (!result.success) {
                guard->finishOldestBoundaryProbe();
                return;
            }

            const bool exists = !result.postIds.isEmpty();
            qCDebug(lcTimelineChannel).nospace()
                << "OLDEST_BOUNDARY_INITIAL_RESULT page=" << page
                << " offset=" << offset
                << " exists=" << exists;

            if (exists) {
                guard->oldestBoundaryNonEmptyPage = page;
            } else {
                guard->oldestBoundaryEmptyPage = page;
            }
            guard->probeOldestBoundary();
        });
}''')

cpp = replace_function(cpp, 'void ChannelPostSource::resolveOldestBoundary(', r'''void ChannelPostSource::resolveOldestBoundary(int emptyPage,
                                              std::function<void()> completion)
{
    if (completion) {
        oldestBoundaryWaiters.push_back(std::move(completion));
    }

    oldestBoundaryFastPathTried = true;
    emptyPage = std::max(0, emptyPage);
    if (oldestBoundaryProbeInFlight) {
        if (oldestBoundaryEmptyPage < 0 || emptyPage < oldestBoundaryEmptyPage) {
            oldestBoundaryEmptyPage = emptyPage;
        }
        return;
    }

    oldestBoundaryProbeInFlight = true;
    oldestBoundaryNonEmptyPage = -1;
    oldestBoundaryEmptyPage = emptyPage;
    oldestBoundaryProbeStep = initialBoundaryProbePages();
    oldestBoundarySearchLimitPage = -1;
    oldestBoundaryMaterializedFullPage = -1;
    probeOldestBoundary();
}

void ChannelPostSource::resolveOldestBoundaryFromNonEmpty(
    int nonEmptyPage, std::function<void()> completion)
{
    if (completion) {
        oldestBoundaryWaiters.push_back(std::move(completion));
    }

    oldestBoundaryFastPathTried = true;
    nonEmptyPage = std::max(0, nonEmptyPage);
    if (oldestBoundaryProbeInFlight) {
        if (oldestBoundaryEmptyPage < 0 || nonEmptyPage < oldestBoundaryEmptyPage) {
            oldestBoundaryNonEmptyPage = std::max(oldestBoundaryNonEmptyPage,
                                                  nonEmptyPage);
            oldestBoundaryMaterializedFullPage = std::max(
                oldestBoundaryMaterializedFullPage, nonEmptyPage);
        }
        return;
    }

    oldestBoundaryProbeInFlight = true;
    oldestBoundaryNonEmptyPage = nonEmptyPage;
    oldestBoundaryEmptyPage = -1;
    // First check the adjacent page. Exact multiples of ten are common, so one
    // cheap probe should prove them before exponential outward stepping begins.
    oldestBoundaryProbeStep = 1;
    oldestBoundarySearchLimitPage = -1;
    oldestBoundaryMaterializedFullPage = nonEmptyPage;
    probeOldestBoundary();
}''')

cpp = replace_function(cpp, 'void ChannelPostSource::probeOldestBoundary()', r'''void ChannelPostSource::probeOldestBoundary()
{
    if (!oldestBoundaryProbeInFlight) {
        return;
    }

    if (oldestBoundaryEmptyPage == 0) {
        reconcileRootCount(0);
        finishOldestBoundaryProbe();
        return;
    }

    // A full materialized page followed immediately by known emptiness proves
    // an exact multiple-of-ten boundary without re-fetching that page.
    if (oldestBoundaryNonEmptyPage >= 0
        && oldestBoundaryEmptyPage == oldestBoundaryNonEmptyPage + 1
        && oldestBoundaryMaterializedFullPage == oldestBoundaryNonEmptyPage) {
        reconcileRootCount(oldestBoundaryEmptyPage * ServerPageSize);
        finishOldestBoundaryProbe();
        return;
    }

    // If the 3% heuristic is only one page, materializing the candidate block
    // is cheaper than maintaining a separate probe phase. Once binary search
    // leaves at most two unknown pages, stop spending one-root probes on them:
    // materialize the first unknown ten-post page instead. A short/empty page
    // resolves the boundary immediately; a full page needs at most one adjacent
    // ten-post page and both payloads are useful to the viewport/prefetch window.
    if ((oldestBoundaryNonEmptyPage < 0 && oldestBoundaryProbeStep == 1)
        || (oldestBoundaryNonEmptyPage >= 0 && oldestBoundaryEmptyPage >= 0
            && oldestBoundaryEmptyPage - oldestBoundaryNonEmptyPage <= 3)) {
        if (oldestBoundaryNonEmptyPage < 0) {
            oldestBoundaryProbeStep = 2;
            loadOldestBoundaryPage(std::max(0, oldestBoundaryEmptyPage - 1));
        } else if (oldestBoundaryEmptyPage == oldestBoundaryNonEmptyPage + 1) {
            loadOldestBoundaryPage(oldestBoundaryNonEmptyPage);
        } else {
            loadOldestBoundaryPage(oldestBoundaryNonEmptyPage + 1);
        }
        return;
    }

    int page = -1;
    if (oldestBoundaryNonEmptyPage >= 0 && oldestBoundaryEmptyPage < 0) {
        if (oldestBoundarySearchLimitPage >= 0) {
            // The initial 3% probe found data. Walk outward toward the reported
            // boundary without assuming it is empty.
            if (oldestBoundaryNonEmptyPage >= oldestBoundarySearchLimitPage) {
                loadOldestBoundaryPage(oldestBoundaryNonEmptyPage);
                return;
            }
            page = oldestBoundaryNonEmptyPage
                + (oldestBoundarySearchLimitPage - oldestBoundaryNonEmptyPage + 1) / 2;
        } else {
            // A full reported-boundary page proves that the count may be too
            // small. Probe one adjacent page first, then grow the outward step
            // exponentially. Existence is monotonic in absolute page space.
            const int step = std::max(1, oldestBoundaryProbeStep);
            const qint64 candidate = static_cast<qint64>(oldestBoundaryNonEmptyPage)
                + step;
            if (candidate > std::numeric_limits<int>::max()) {
                finishOldestBoundaryProbe();
                return;
            }
            page = static_cast<int>(candidate);
            if (step == 1) {
                oldestBoundaryProbeStep = std::max(2, initialBoundaryProbePages());
            } else {
                oldestBoundaryProbeStep = std::min(
                    std::numeric_limits<int>::max() / 2, step * 2);
            }
        }
    } else if (oldestBoundaryNonEmptyPage < 0) {
        page = std::max(0, oldestBoundaryEmptyPage - oldestBoundaryProbeStep);
        oldestBoundaryProbeStep = std::min(oldestBoundaryEmptyPage + 1,
                                           oldestBoundaryProbeStep * 2);
    } else {
        page = oldestBoundaryNonEmptyPage
            + (oldestBoundaryEmptyPage - oldestBoundaryNonEmptyPage) / 2;
    }

    const int offset = page * ServerPageSize;
    qCDebug(lcTimelineChannel).nospace()
        << "OLDEST_BOUNDARY_PROBE page=" << page
        << " offset=" << offset
        << " nonEmpty=" << oldestBoundaryNonEmptyPage
        << " empty=" << oldestBoundaryEmptyPage
        << " step=" << oldestBoundaryProbeStep
        << " perPage=1";

    QPointer<ChannelPostSource> guard(this);
    PostTimelineService::instance(backend).loadChannelPage(
        channel, offset, 1,
        [guard, page, offset](const PostTimelineService::Page& result) {
            if (!guard || !guard->oldestBoundaryProbeInFlight) {
                return;
            }
            if (!result.success) {
                guard->finishOldestBoundaryProbe();
                return;
            }

            const bool exists = !result.postIds.isEmpty();
            qCDebug(lcTimelineChannel).nospace()
                << "OLDEST_BOUNDARY_RESULT page=" << page
                << " offset=" << offset
                << " exists=" << exists;

            if (exists) {
                guard->oldestBoundaryNonEmptyPage = std::max(
                    guard->oldestBoundaryNonEmptyPage, page);
            } else if (guard->oldestBoundaryEmptyPage < 0) {
                guard->oldestBoundaryEmptyPage = page;
            } else {
                guard->oldestBoundaryEmptyPage = std::min(
                    guard->oldestBoundaryEmptyPage, page);
            }
            guard->probeOldestBoundary();
        });
}''')

cpp = replace_function(cpp, 'void ChannelPostSource::loadOldestBoundaryPage(int page)', r'''void ChannelPostSource::loadOldestBoundaryPage(int page)
{
    if (!oldestBoundaryProbeInFlight) {
        return;
    }

    page = std::max(0, page);
    qCDebug(lcTimelineChannel).nospace()
        << "OLDEST_BOUNDARY_PAGE page=" << page
        << " perPage=" << ServerPageSize;

    QPointer<ChannelPostSource> guard(this);
    PostTimelineService::instance(backend).loadChannelPage(
        channel, page, ServerPageSize,
        [guard, page](const PostTimelineService::Page& result) {
            if (!guard || !guard->oldestBoundaryProbeInFlight) {
                return;
            }
            if (!result.success) {
                guard->finishOldestBoundaryProbe();
                return;
            }

            qCDebug(lcTimelineChannel).nospace()
                << "OLDEST_BOUNDARY_PAGE_RESULT page=" << page
                << " returned=" << result.postIds.size();

            if (result.postIds.isEmpty()) {
                if (guard->oldestBoundaryEmptyPage < 0) {
                    guard->oldestBoundaryEmptyPage = page;
                } else {
                    guard->oldestBoundaryEmptyPage = std::min(
                        guard->oldestBoundaryEmptyPage, page);
                }
                if (guard->oldestBoundaryNonEmptyPage >= page) {
                    guard->oldestBoundaryNonEmptyPage = -1;
                    guard->oldestBoundaryMaterializedFullPage = -1;
                }
                if (guard->oldestBoundaryNonEmptyPage >= 0
                    && guard->oldestBoundaryEmptyPage
                        == guard->oldestBoundaryNonEmptyPage + 1) {
                    if (guard->oldestBoundaryMaterializedFullPage
                        == guard->oldestBoundaryNonEmptyPage) {
                        guard->reconcileRootCount(
                            guard->oldestBoundaryEmptyPage * ServerPageSize);
                        guard->finishOldestBoundaryProbe();
                    } else {
                        guard->loadOldestBoundaryPage(
                            guard->oldestBoundaryNonEmptyPage);
                    }
                    return;
                }
                guard->probeOldestBoundary();
                return;
            }

            guard->oldestBoundaryNonEmptyPage = std::max(
                guard->oldestBoundaryNonEmptyPage, page);
            const int returned = static_cast<int>(result.postIds.size());
            if (returned < ServerPageSize) {
                guard->placePage(page, result.postIds);
                guard->finishOldestBoundaryProbe();
                return;
            }

            guard->placePage(page, result.postIds);
            guard->oldestBoundaryMaterializedFullPage = page;
            if (guard->oldestBoundaryEmptyPage == page + 1) {
                guard->reconcileRootCount((page + 1) * ServerPageSize);
                guard->finishOldestBoundaryProbe();
                return;
            }
            if (guard->oldestBoundaryEmptyPage == page + 2) {
                guard->loadOldestBoundaryPage(page + 1);
                return;
            }

            // Reaching a full reported oldest page is not a boundary proof:
            // total_msg_count_root excludes join/leave and some other visible
            // system roots. Switch from the estimated limit to outward
            // exponential probing until /posts itself proves the edge.
            if (guard->oldestBoundaryEmptyPage < 0) {
                if (guard->oldestBoundarySearchLimitPage >= 0
                    && page >= guard->oldestBoundarySearchLimitPage) {
                    guard->oldestBoundarySearchLimitPage = -1;
                    guard->oldestBoundaryProbeStep = 1;
                }
                guard->probeOldestBoundary();
                return;
            }
            guard->probeOldestBoundary();
        });
}''')

cpp = replace_function(cpp, 'void ChannelPostSource::finishOldestBoundaryProbe()', r'''void ChannelPostSource::finishOldestBoundaryProbe()
{
    auto waiters = std::move(oldestBoundaryWaiters);
    oldestBoundaryWaiters.clear();
    oldestBoundaryProbeInFlight = false;
    oldestBoundaryNonEmptyPage = -1;
    oldestBoundaryEmptyPage = -1;
    oldestBoundaryProbeStep = 1;
    oldestBoundarySearchLimitPage = -1;
    oldestBoundaryMaterializedFullPage = -1;

    for (auto& waiter : waiters) {
        if (waiter) {
            waiter();
        }
    }
}''')

cpp = replace_function(cpp, 'void ChannelPostSource::reconcileRootCount(int actualCount)', r'''void ChannelPostSource::reconcileRootCount(int actualCount)
{
    actualCount = std::max(0, actualCount);
    const int oldCount = static_cast<int>(postIds.size());

    // Preserve the discovered difference from Mattermost's message counter so
    // later metadata refreshes do not recreate a deleted-root phantom prefix or
    // discard count-excluded system roots. The server counter can subsequently
    // advance with normal messages; this signed adjustment advances with it.
    rootCountAdjustment = actualCount - std::max(0, channel.total_msg_count_root);

    if (actualCount == oldCount) {
        return;
    }

    if (actualCount > oldCount) {
        const int addPrefix = actualCount - oldCount;
        qCDebug(lcTimelineChannel).nospace()
            << "OLDEST_COUNT_RECONCILE addPrefix=" << addPrefix
            << " oldCount=" << oldCount
            << " actualCount=" << actualCount;
        insertLogicalPrefix(addPrefix);
        return;
    }

    const int removePrefix = oldCount - actualCount;
    qCDebug(lcTimelineChannel).nospace()
        << "OLDEST_COUNT_RECONCILE removePrefix=" << removePrefix
        << " oldCount=" << oldCount
        << " actualCount=" << actualCount;
    removeLogicalRange(0, removePrefix);
}

void ChannelPostSource::ensureMinimumRootCount(int minimumCount)
{
    minimumCount = std::max(0, minimumCount);
    const int oldCount = static_cast<int>(postIds.size());
    if (minimumCount <= oldCount) {
        return;
    }

    const int addPrefix = minimumCount - oldCount;
    qCDebug(lcTimelineChannel).nospace()
        << "OLDEST_COUNT_EXPAND addPrefix=" << addPrefix
        << " oldCount=" << oldCount
        << " minimumCount=" << minimumCount;
    insertLogicalPrefix(addPrefix);
}

void ChannelPostSource::insertLogicalPrefix(int count)
{
    count = std::max(0, count);
    if (count == 0) {
        return;
    }

    if (provisionalWindow.isValid()) {
        provisionalWindow.first += count;
    }

    QVector<QString> next;
    next.reserve(count + static_cast<int>(postIds.size()));
    for (int i = 0; i < count; ++i) {
        next.push_back(QString());
    }
    for (const QString& id : std::as_const(postIds)) {
        next.push_back(id);
    }
    postIds = std::move(next);
    rebuildIndex();
    emit itemsInserted(0, count);
}''')

cpp = replace_function(cpp, 'int ChannelPostSource::currentLogicalCount() const', r'''int ChannelPostSource::currentLogicalCount() const
{
    if (hasRootCountEstimate) {
        const int adjustedServerCount = std::max(
            0, channel.total_msg_count_root + rootCountAdjustment);
        return std::max(static_cast<int>(postIds.size()), adjustedServerCount);
    }
    return static_cast<int>(postIds.size());
}''')

cpp = replace_function(cpp, 'void ChannelPostSource::placePage(int page, const QStringList& chronologicalIds)', r'''void ChannelPostSource::placePage(int page, const QStringList& chronologicalIds)
{
    if (!hasRootCountEstimate || postIds.isEmpty() || chronologicalIds.isEmpty()) {
        return;
    }

    const int returned = static_cast<int>(chronologicalIds.size());

    // Absolute /posts pages are the timeline authority. A short page proves the
    // exact oldest boundary. A full page only proves a minimum count; grow at
    // the oldest side so already known newest-anchored page mappings keep their
    // indices. This handles total_msg_count_root underestimates caused by system
    // roots that are visible in /posts but excluded from channel message counts.
    if (returned < ServerPageSize) {
        reconcileRootCount(page * ServerPageSize + returned);
    } else {
        ensureMinimumRootCount((page + 1) * ServerPageSize);
    }

    const int first = std::max(0,
        static_cast<int>(postIds.size()) - page * ServerPageSize - returned);

    const int count = std::min(returned,
                               static_cast<int>(postIds.size()) - first);
    if (count <= 0) {
        return;
    }
    const int last = first + count - 1;

    bool touchesProvisionalIdentity = false;
    bool mappingChanged = false;
    QSet<int> concreteChanged;

    for (int offset = 0; offset < count; ++offset) {
        const QString& id = chronologicalIds.at(offset);
        if (provisionalPostIds.contains(id)) {
            touchesProvisionalIdentity = true;
        }
        const int existing = postIndexes.value(id, -1);
        if (existing >= 0 && (existing < first || existing > last)
            && !postIds.at(existing).isEmpty()) {
            postIds[existing].clear();
            concreteChanged.insert(existing);
            mappingChanged = true;
        }
    }

    for (int offset = 0; offset < count; ++offset) {
        const int index = first + offset;
        const QString& id = chronologicalIds.at(offset);
        if (postIds.at(index) != id) {
            if (!postIds.at(index).isEmpty()) {
                concreteChanged.insert(index);
            }
            postIds[index] = id;
            mappingChanged = true;
        }
        provisionalPostIds.remove(id);
    }
    rebuildIndex();

    // An absolute page that happens to intersect the provisional island by ID
    // provides the missing exact offset. Re-adopt the whole local context before
    // publishing page changes so the target never disappears between states.
    if (touchesProvisionalIdentity && provisionalWindow.isValid()) {
        const ProvisionalWindow window = provisionalWindow;
        placeNavigationContext(window.targetPostId, window.postIds,
                               window.reachedOldest, window.reachedNewest);
    }

    // Re-fetching an already known page must be a no-op. In particular, do not
    // emit rangeAvailable/itemsChanged for identical identities: both signals
    // schedule another synchronization, which used to clear request suppression
    // and immediately ask for the same impossible oldest range again.
    if (!mappingChanged) {
        return;
    }

    for (int index : std::as_const(concreteChanged)) {
        emit itemsChanged(index, index);
    }
    emit rangeAvailable(first, last);
}''')

cpp_path.write_text(cpp)

h = h_path.read_text()
h = h.replace('exactRootCount', 'hasRootCountEstimate')
h = h.replace('rootCountOverestimate', 'rootCountAdjustment')
h = h.replace(
    '    void resolveOldestBoundary(int emptyPage, std::function<void()> completion);\n',
    '    void resolveOldestBoundary(int emptyPage, std::function<void()> completion);\n'
    '    void resolveOldestBoundaryFromNonEmpty(int nonEmptyPage,\n'
    '                                           std::function<void()> completion);\n')
h = h.replace(
    '    void reconcileRootCount(int actualCount);\n',
    '    void reconcileRootCount(int actualCount);\n'
    '    void ensureMinimumRootCount(int minimumCount);\n'
    '    void insertLogicalPrefix(int count);\n')
h = h.replace(
    '    const bool hasRootCountEstimate;\n    // total_msg_count_root can overestimate /posts because deleted roots are\n'
    '    // omitted. Empty/short absolute pages resolve the real oldest boundary;\n'
    '    // keep that correction local so later channel metadata cannot recreate the\n'
    '    // phantom logical prefix.\n    int rootCountAdjustment = 0;\n',
    '    const bool hasRootCountEstimate;\n'
    '    // total_msg_count_root is a message-count estimate, not /posts row count.\n'
    '    // Deleted roots can make it too large; count-excluded system roots can\n'
    '    // make it too small. Once /posts proves the exact oldest boundary, keep\n'
    '    // the signed difference so later metadata refreshes preserve that mapping.\n'
    '    int rootCountAdjustment = 0;\n')
h = h.replace(
    '    int oldestBoundarySearchLimitPage = -1;\n',
    '    int oldestBoundarySearchLimitPage = -1;\n'
    '    int oldestBoundaryMaterializedFullPage = -1;\n')
h = h.replace(
    '    // Large-channel top-edge search starts this far inside the estimated count.\n'
    '    // This is a latency heuristic only; outward binary search or inward\n'
    '    // exponential fallback preserves correctness.\n',
    '    // Large-channel top-edge search starts this far inside the estimated count.\n'
    '    // This is a latency heuristic only; inward/outward boundary search keeps\n'
    '    // correctness independent of whether the estimate is high or low.\n')
h_path.write_text(h)

long_doc = long_doc_path.read_text()
old = '''`total_msg_count_root` may overestimate the rows returned by `/posts` because deleted roots can be
omitted from history. If an absolute page calculated from that estimate is successfully fetched but
empty, the source treats the empty response as boundary evidence instead of leaving a black logical
range. Boundary search stays in ten-post page coordinates but tests distant candidate pages with one
root only: candidate page P is probed as `page=P*10&per_page=1`, which asks whether the first offset
of that ten-post page exists.

For a large top-edge request the source does not spend a first round trip validating the estimated
oldest page. Its first one-root probe jumps inward by a heuristic 3% of the estimated root count,
rounded to ten-post pages. This ratio affects latency only; correctness does not depend on it. If that
probe is empty, the step grows exponentially farther inward until data is found. If it already finds
data, binary search walks outward towards the estimated oldest page without assuming that estimate is
empty; the estimated page itself is fetched only if the search actually reaches it. If the 3% distance
is no larger than one page, the normal ten-post path is used instead because a small channel is cheap
to materialize.

Near the end of binary search, when at most two unknown ten-post pages remain, the source stops
spending one-root probes on that tiny interval and fetches the first unknown page with `per_page=10`.
A short result proves the exact oldest boundary immediately; an empty result tightens the boundary; and
a full page requires at most one adjacent ten-post page to finish. In every case returned posts are
already useful viewport/prefetch materialization. The phantom logical prefix is removed once the exact
count is known and normal ten-post paging continues. No identity cursor is introduced by this
reconciliation path.
'''
new = '''`total_msg_count_root` is only an initial coordinate estimate for `/posts`, not its row count.
Deleted roots can make the counter larger than visible history, while join/leave and other system
roots excluded from Mattermost message counts are still returned by `/posts` and can make the counter
smaller. The source therefore repairs the oldest boundary in both directions. Absolute pages remain
newest-anchored; count growth inserts empty logical slots at the oldest side so already mapped pages do
not move relative to the newest edge.

Boundary search stays in ten-post page coordinates but tests distant candidate page starts with one
root only: candidate page P is probed as `page=P*10&per_page=1`. For a large top-edge request the first
probe jumps inward by a heuristic 3% of the estimated root count. If it is empty, the step grows
exponentially farther inward until data is found. If it exists, binary search walks outward to the
reported boundary. A full reported-boundary page is not accepted as proof: the source first probes the
adjacent older page and, if that exists too, expands outward exponentially until `/posts` provides an
empty/short boundary. Small estimates use the normal ten-post path first and enter the same outward
repair only when their reported oldest page is full.

Near a bounded edge, when at most two unknown ten-post pages remain, the source stops spending
one-root probes and materializes the first unknown page with `per_page=10`. A short page proves the
exact count; a full page adjacent to known emptiness proves an exact multiple of ten. Exact
reconciliation may therefore remove a phantom prefix or insert a missing oldest prefix. The 3% value
changes latency only, never correctness, and no identity cursor is introduced by this repair path.
'''
if old not in long_doc:
    raise RuntimeError('long-list boundary text not found')
long_doc_path.write_text(long_doc.replace(old, new))

cache_doc = cache_doc_path.read_text()
old = '''page is also authoritative boundary evidence: when `total_msg_count_root` may overstate `/posts`
history, a large top-edge request first validates the estimated oldest page with `per_page=1` instead
of downloading guessed ten-post pages. An empty validation starts distant probing at a heuristic 3% of
the estimated root count; candidate page starts use `per_page=1` until the search becomes local, where
the page immediately before known emptiness is materialized with ten posts. The source then removes any
phantom logical prefix and keeps ten-post paging for visible/prefetched history. The 3% value is a latency
heuristic, never a correctness assumption.
'''
new = '''page is also authoritative boundary evidence. `total_msg_count_root` can overstate `/posts` when
deleted roots disappear, but it can also understate it because join/leave and other count-excluded
system roots remain visible in channel history. A large top-edge request starts with a one-root probe
3% inside the estimate. Empty results search inward; existing data searches outward to the estimate
and, when the reported oldest page is full, continues beyond it until `/posts` proves the real edge.
Small estimates use a normal ten-post page first and expand outward if that page disproves the count.
Exact reconciliation removes or inserts an oldest logical prefix while preserving newest-anchored page
mapping. The 3% value is a latency heuristic, never a correctness assumption.
'''
if old not in cache_doc:
    raise RuntimeError('post-cache boundary text not found')
cache_doc = cache_doc.replace(old, new)
cache_doc = cache_doc.replace(
    '`total_msg_count_root` is useful for scrollbar\nscale and for choosing an initial random-seek page, but deleted roots and\nconcurrent server changes can make the count disagree with the rows returned\nby `/channels/{id}/posts`.\n',
    '`total_msg_count_root` is useful for scrollbar scale and for choosing an initial random-seek\npage, but it is not `/posts` row count: deleted roots can make it too large, while join/leave and\nother count-excluded system roots can make it too small. Concurrent server changes can add another\nsource of disagreement.\n')
cache_doc_path.write_text(cache_doc)

thread_doc = thread_doc_path.read_text()
thread_doc = thread_doc.replace(
    'Channel history needs an absolute-page boundary repair because `total_msg_count_root` may count rows\nthat ordinary `/channels/{id}/posts` history does not return. Threads deliberately do not inherit that\npage-probing algorithm. Mattermost `Thread.ReplyCount` excludes deleted replies, and the thread endpoint\n',
    'Channel history needs an absolute-page boundary repair because `total_msg_count_root` is not the\nrow count of ordinary `/channels/{id}/posts`: deleted roots can make it too large, while count-excluded\nsystem roots can make it too small. Threads deliberately do not inherit that page-probing algorithm.\nMattermost `Thread.ReplyCount` excludes deleted replies, and the thread endpoint\n')
thread_doc_path.write_text(thread_doc)

print('symmetric channel-boundary patch applied')
