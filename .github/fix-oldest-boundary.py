from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if old not in text:
        raise SystemExit(f"{label} anchor not found")
    return text.replace(old, new, 1)


source_path = Path("sources/chat-area/ChannelPostSource.cpp")
source = source_path.read_text()

source = replace_once(
    source,
    '''        PostTimelineService::instance(backend).loadChannelPage(
            channel, page, ServerPageSize,
            [guard, page, finishPage](const PostTimelineService::Page& result) {
                if (!guard) {
                    return;
                }
                if (result.success && !result.postIds.isEmpty()) {
                    guard->placePage(page, result.postIds);
                }
                finishPage();
            });
    }

}

bool ChannelPostSource::canRequestBeforeFirst() const
''',
    '''        PostTimelineService::instance(backend).loadChannelPage(
            channel, page, ServerPageSize,
            [guard, page, finishPage](const PostTimelineService::Page& result) {
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
                finishPage();
            });
    }
}

void ChannelPostSource::resolveOldestBoundary(int emptyPage,
                                              std::function<void()> completion)
{
    if (completion) {
        oldestBoundaryWaiters.push_back(std::move(completion));
    }
    if (oldestBoundaryProbeInFlight) {
        return;
    }

    oldestBoundaryProbeInFlight = true;
    oldestBoundaryNonEmptyPage = -1;
    oldestBoundaryEmptyPage = std::max(0, emptyPage);
    oldestBoundaryProbeStep = 1;
    oldestBoundaryNonEmptyIds.clear();
    probeOldestBoundary();
}

void ChannelPostSource::probeOldestBoundary()
{
    if (!oldestBoundaryProbeInFlight) {
        return;
    }

    if (oldestBoundaryEmptyPage == 0) {
        reconcileRootCount(0, 0, 0);
        finishOldestBoundaryProbe();
        return;
    }

    if (oldestBoundaryNonEmptyPage >= 0
        && oldestBoundaryEmptyPage == oldestBoundaryNonEmptyPage + 1) {
        const QStringList ids = oldestBoundaryNonEmptyIds;
        const int page = oldestBoundaryNonEmptyPage;
        reconcileRootCount(page * ServerPageSize + static_cast<int>(ids.size()),
                           page, static_cast<int>(ids.size()));
        placePage(page, ids);
        finishOldestBoundaryProbe();
        return;
    }

    int page = -1;
    if (oldestBoundaryNonEmptyPage < 0) {
        page = std::max(0, oldestBoundaryEmptyPage - oldestBoundaryProbeStep);
        oldestBoundaryProbeStep = std::min(oldestBoundaryEmptyPage + 1,
                                           oldestBoundaryProbeStep * 2);
    } else {
        page = oldestBoundaryNonEmptyPage
            + (oldestBoundaryEmptyPage - oldestBoundaryNonEmptyPage) / 2;
    }

    qCDebug(lcTimelineChannel).nospace()
        << "OLDEST_BOUNDARY_PROBE page=" << page
        << " nonEmpty=" << oldestBoundaryNonEmptyPage
        << " empty=" << oldestBoundaryEmptyPage
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
                << "OLDEST_BOUNDARY_RESULT page=" << page
                << " returned=" << result.postIds.size();

            if (result.postIds.isEmpty()) {
                guard->oldestBoundaryEmptyPage = page;
                guard->probeOldestBoundary();
                return;
            }

            guard->oldestBoundaryNonEmptyPage = page;
            guard->oldestBoundaryNonEmptyIds = result.postIds;
            if (result.postIds.size() < ServerPageSize) {
                guard->reconcileRootCount(
                    page * ServerPageSize + static_cast<int>(result.postIds.size()),
                    page, static_cast<int>(result.postIds.size()));
                guard->placePage(page, result.postIds);
                guard->finishOldestBoundaryProbe();
                return;
            }
            guard->probeOldestBoundary();
        });
}

void ChannelPostSource::finishOldestBoundaryProbe()
{
    auto waiters = std::move(oldestBoundaryWaiters);
    oldestBoundaryWaiters.clear();
    oldestBoundaryProbeInFlight = false;
    oldestBoundaryNonEmptyPage = -1;
    oldestBoundaryEmptyPage = -1;
    oldestBoundaryProbeStep = 1;
    oldestBoundaryNonEmptyIds.clear();

    for (auto& waiter : waiters) {
        if (waiter) {
            waiter();
        }
    }
}

void ChannelPostSource::reconcileRootCount(int actualCount,
                                           int page,
                                           int returnedCount)
{
    actualCount = std::max(0, actualCount);
    const int removePrefix = static_cast<int>(postIds.size()) - actualCount;
    if (removePrefix <= 0) {
        return;
    }

    qCDebug(lcTimelineChannel).nospace()
        << "OLDEST_COUNT_RECONCILE page=" << page
        << " removePrefix=" << removePrefix
        << " oldCount=" << postIds.size()
        << " actualCount=" << actualCount
        << " returned=" << returnedCount;
    rootCountOverestimate += removePrefix;
    removeLogicalRange(0, removePrefix);
}

bool ChannelPostSource::canRequestBeforeFirst() const
''',
    "requestRange",
)

source = replace_once(
    source,
    '''    int first = std::max(0,
        static_cast<int>(postIds.size()) - page * ServerPageSize
            - static_cast<int>(chronologicalIds.size()));

    // /channels/{id}/posts omits deleted posts, while total_msg_count_root can
    // still leave the source with a larger logical estimate. A short non-zero
    // page is the oldest boundary, so a positive calculated first index is not a
    // real gap: it is exactly the number of phantom deleted-root slots. Remove
    // them structurally before publishing the page. For the reported 1070-row
    // DM, page 106 contained seven posts and proved that indices 0..2 did not
    // exist, which was the trigger for the infinite [0,9] seek/reload loop.
    if (page > 0
        && chronologicalIds.size() < ServerPageSize
        && first > 0) {
        qCDebug(lcTimelineChannel).nospace()
            << "OLDEST_COUNT_RECONCILE page=" << page
            << " removePrefix=" << first
            << " oldCount=" << postIds.size()
            << " returned=" << chronologicalIds.size();
        rootCountOverestimate += first;
        removeLogicalRange(0, first);
        first = std::max(0,
            static_cast<int>(postIds.size()) - page * ServerPageSize
                - static_cast<int>(chronologicalIds.size()));
    }
''',
    '''    // /channels/{id}/posts omits deleted roots while total_msg_count_root
    // may continue counting them. A short absolute page is authoritative proof
    // of the real oldest boundary, including page zero for small channels.
    if (chronologicalIds.size() < ServerPageSize) {
        reconcileRootCount(
            page * ServerPageSize + static_cast<int>(chronologicalIds.size()),
            page, static_cast<int>(chronologicalIds.size()));
    }

    const int first = std::max(0,
        static_cast<int>(postIds.size()) - page * ServerPageSize
            - static_cast<int>(chronologicalIds.size()));
''',
    "placePage",
)
source_path.write_text(source)

header_path = Path("sources/chat-area/ChannelPostSource.h")
header = header_path.read_text()
header = replace_once(
    header,
    '''#pragma once

#include <QHash>
''',
    '''#pragma once

#include <functional>
#include <vector>

#include <QHash>
''',
    "header includes",
)
header = replace_once(
    header,
    '''    void removeLogicalRange(int first, int count);
    void placePage(int page, const QStringList& chronologicalIds);
    void prependDiscovered(const QStringList& chronologicalIds);
''',
    '''    void removeLogicalRange(int first, int count);
    void placePage(int page, const QStringList& chronologicalIds);
    void resolveOldestBoundary(int emptyPage, std::function<void()> completion);
    void probeOldestBoundary();
    void finishOldestBoundaryProbe();
    void reconcileRootCount(int actualCount, int page, int returnedCount);
    void prependDiscovered(const QStringList& chronologicalIds);
''',
    "header methods",
)
header = replace_once(
    header,
    '''    int rootCountOverestimate = 0;
    bool moreBeforeFirst = false;
    bool beforeRequestInFlight = false;

    ProvisionalWindow provisionalWindow;
''',
    '''    int rootCountOverestimate = 0;
    bool moreBeforeFirst = false;
    bool beforeRequestInFlight = false;

    bool oldestBoundaryProbeInFlight = false;
    int oldestBoundaryNonEmptyPage = -1;
    int oldestBoundaryEmptyPage = -1;
    int oldestBoundaryProbeStep = 1;
    QStringList oldestBoundaryNonEmptyIds;
    std::vector<std::function<void()>> oldestBoundaryWaiters;

    ProvisionalWindow provisionalWindow;
''',
    "header state",
)
header_path.write_text(header)

long_list_path = Path("docs/long-list-architecture.md")
long_list = long_list_path.read_text()
long_list = replace_once(
    long_list,
    '''ten-item logical request may intersect two server pages; the source loads both and places each via
its absolute page number. A jump to any scrollbar position is therefore O(1) page requests rather
than an identity-cursor walk through history.
''',
    '''ten-item logical request may intersect two server pages; the source loads both and places each via
its absolute page number. Once the oldest boundary is known, a jump to any scrollbar position is
therefore O(1) page requests rather than an identity-cursor walk through history.

`total_msg_count_root` may overestimate the rows returned by `/posts` because deleted roots can be
omitted from history. If an absolute page calculated from that estimate is successfully fetched but
empty, the source treats the empty response as boundary evidence instead of leaving a black logical
range. It probes older absolute pages with exponentially increasing steps until it finds data, then
binary-searches the remaining page interval. A short page, or a full page immediately followed by an
empty page, proves the real oldest boundary. The phantom logical prefix is then removed once and all
subsequent range requests use the corrected O(1) mapping. No identity cursor is introduced by this
reconciliation path, and every probe still uses `per_page=10`.
''',
    "long-list docs",
)
long_list_path.write_text(long_list)

post_cache_path = Path("docs/post-cache.md")
post_cache = post_cache_path.read_text()
post_cache = replace_once(
    post_cache,
    '''Remote thumb
seek, normal scrolling and initial tail materialization therefore share exactly the same paging
path instead of switching between page arithmetic and cursor walks.
''',
    '''Remote thumb
seek, normal scrolling and initial tail materialization therefore share exactly the same paging
path instead of switching between page arithmetic and cursor walks. A successful empty absolute
page is also authoritative boundary evidence: when `total_msg_count_root` overstates `/posts`
history, the source resolves the real oldest page with absolute ten-post probes, removes the phantom
logical prefix, and keeps using the corrected page arithmetic afterward.
''',
    "post-cache docs",
)
post_cache_path.write_text(post_cache)
