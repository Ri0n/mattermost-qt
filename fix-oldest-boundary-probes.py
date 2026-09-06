from pathlib import Path
import re


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if old not in text:
        raise SystemExit(f"{label} anchor not found")
    return text.replace(old, new, 1)


source_path = Path("sources/chat-area/ChannelPostSource.cpp")
source = source_path.read_text()

pattern = re.compile(
    r"void ChannelPostSource::resolveOldestBoundary\(int emptyPage,.*?\n"
    r"bool ChannelPostSource::canRequestBeforeFirst\(\) const\n",
    re.S,
)
replacement = '''void ChannelPostSource::resolveOldestBoundary(int emptyPage,
                                              std::function<void()> completion)
{
    if (completion) {
        oldestBoundaryWaiters.push_back(std::move(completion));
    }

    // page=N with per_page=1 addresses exactly the Nth root from the newest
    // edge. Convert the empty ten-post page into an empty-offset upper bound and
    // resolve the exact boundary with one-post probes. The ten-post page size is
    // reserved for materialization/prefetch, not boundary discovery.
    const int emptyOffset = std::max(0, emptyPage) * ServerPageSize;
    if (oldestBoundaryProbeInFlight) {
        if (oldestBoundaryEmptyOffset < 0 || emptyOffset < oldestBoundaryEmptyOffset) {
            oldestBoundaryEmptyOffset = emptyOffset;
        }
        return;
    }

    oldestBoundaryProbeInFlight = true;
    oldestBoundaryNonEmptyOffset = -1;
    oldestBoundaryEmptyOffset = emptyOffset;
    oldestBoundaryProbeStep = 1;
    probeOldestBoundary();
}

void ChannelPostSource::probeOldestBoundary()
{
    if (!oldestBoundaryProbeInFlight) {
        return;
    }

    if (oldestBoundaryEmptyOffset == 0) {
        reconcileRootCount(0);
        finishOldestBoundaryProbe();
        return;
    }

    if (oldestBoundaryNonEmptyOffset >= 0
        && oldestBoundaryEmptyOffset == oldestBoundaryNonEmptyOffset + 1) {
        const int actualCount = oldestBoundaryEmptyOffset;
        reconcileRootCount(actualCount);

        // Probes establish only the count. Once the coordinate system is exact,
        // fetch the real oldest visible block with the normal ten-post policy.
        const int page = (actualCount - 1) / ServerPageSize;
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
                if (result.success && !result.postIds.isEmpty()) {
                    guard->placePage(page, result.postIds);
                }
                guard->finishOldestBoundaryProbe();
            });
        return;
    }

    int offset = -1;
    if (oldestBoundaryNonEmptyOffset < 0) {
        offset = std::max(0, oldestBoundaryEmptyOffset - oldestBoundaryProbeStep);
        oldestBoundaryProbeStep = std::min(oldestBoundaryEmptyOffset + 1,
                                           oldestBoundaryProbeStep * 2);
    } else {
        offset = oldestBoundaryNonEmptyOffset
            + (oldestBoundaryEmptyOffset - oldestBoundaryNonEmptyOffset) / 2;
    }

    qCDebug(lcTimelineChannel).nospace()
        << "OLDEST_BOUNDARY_PROBE offset=" << offset
        << " nonEmpty=" << oldestBoundaryNonEmptyOffset
        << " empty=" << oldestBoundaryEmptyOffset
        << " perPage=1";

    QPointer<ChannelPostSource> guard(this);
    PostTimelineService::instance(backend).loadChannelPage(
        channel, offset, 1,
        [guard, offset](const PostTimelineService::Page& result) {
            if (!guard || !guard->oldestBoundaryProbeInFlight) {
                return;
            }
            if (!result.success) {
                guard->finishOldestBoundaryProbe();
                return;
            }

            const bool exists = !result.postIds.isEmpty();
            qCDebug(lcTimelineChannel).nospace()
                << "OLDEST_BOUNDARY_RESULT offset=" << offset
                << " exists=" << exists;

            if (exists) {
                guard->oldestBoundaryNonEmptyOffset = std::max(
                    guard->oldestBoundaryNonEmptyOffset, offset);
            } else {
                guard->oldestBoundaryEmptyOffset = std::min(
                    guard->oldestBoundaryEmptyOffset, offset);
            }
            guard->probeOldestBoundary();
        });
}

void ChannelPostSource::finishOldestBoundaryProbe()
{
    auto waiters = std::move(oldestBoundaryWaiters);
    oldestBoundaryWaiters.clear();
    oldestBoundaryProbeInFlight = false;
    oldestBoundaryNonEmptyOffset = -1;
    oldestBoundaryEmptyOffset = -1;
    oldestBoundaryProbeStep = 1;

    for (auto& waiter : waiters) {
        if (waiter) {
            waiter();
        }
    }
}

void ChannelPostSource::reconcileRootCount(int actualCount)
{
    actualCount = std::max(0, actualCount);
    const int removePrefix = static_cast<int>(postIds.size()) - actualCount;
    if (removePrefix <= 0) {
        return;
    }

    qCDebug(lcTimelineChannel).nospace()
        << "OLDEST_COUNT_RECONCILE removePrefix=" << removePrefix
        << " oldCount=" << postIds.size()
        << " actualCount=" << actualCount;
    rootCountOverestimate += removePrefix;
    removeLogicalRange(0, removePrefix);
}

bool ChannelPostSource::canRequestBeforeFirst() const
'''
source, count = pattern.subn(replacement, source, count=1)
if count != 1:
    raise SystemExit("boundary resolver block not found")

source = replace_once(
    source,
    '''        reconcileRootCount(
            page * ServerPageSize + static_cast<int>(chronologicalIds.size()),
            page, static_cast<int>(chronologicalIds.size()));
''',
    '''        reconcileRootCount(
            page * ServerPageSize + static_cast<int>(chronologicalIds.size()));
''',
    "placePage reconcile",
)
source_path.write_text(source)

header_path = Path("sources/chat-area/ChannelPostSource.h")
header = header_path.read_text()
header = replace_once(
    header,
    '''    // Channel history range loading always uses Mattermost's ten-post absolute
    // pages. Identity cursors are reserved for semantic/compatibility paths and
    // never define ordinary scroll or seek request boundaries.
''',
    '''    // Visible/prefetch channel ranges use Mattermost's ten-post absolute
    // pages. Boundary discovery deliberately uses per_page=1 probes so it does
    // not fetch viewport-sized payloads merely to test whether an offset exists.
''',
    "page-size comment",
)
header = replace_once(
    header,
    '''    void reconcileRootCount(int actualCount, int page, int returnedCount);
''',
    '''    void reconcileRootCount(int actualCount);
''',
    "reconcile declaration",
)
header = replace_once(
    header,
    '''    int oldestBoundaryNonEmptyPage = -1;
    int oldestBoundaryEmptyPage = -1;
    int oldestBoundaryProbeStep = 1;
    QStringList oldestBoundaryNonEmptyIds;
''',
    '''    int oldestBoundaryNonEmptyOffset = -1;
    int oldestBoundaryEmptyOffset = -1;
    int oldestBoundaryProbeStep = 1;
''',
    "boundary state",
)
header_path.write_text(header)

long_list_path = Path("docs/long-list-architecture.md")
long_list = long_list_path.read_text()
long_list = replace_once(
    long_list,
    '''range. It probes older absolute pages with exponentially increasing steps until it finds data, then
binary-searches the remaining page interval. A short page, or a full page immediately followed by an
empty page, proves the real oldest boundary. The phantom logical prefix is then removed once and all
subsequent range requests use the corrected O(1) mapping. No identity cursor is introduced by this
reconciliation path, and every probe still uses `per_page=10`.
''',
    '''range. Boundary discovery switches to `per_page=1`: in that coordinate system `page=N` asks only
whether the root at newest-offset N exists. The source probes backward from the empty ten-post page
with exponentially increasing offset steps, then binary-searches the remaining item interval. The
first empty one-post offset is the exact real root count. The phantom logical prefix is removed once,
then the actual oldest viewport block is fetched with the normal `per_page=10` policy. No identity
cursor is introduced by this reconciliation path.
''',
    "long-list boundary docs",
)
long_list_path.write_text(long_list)

post_cache_path = Path("docs/post-cache.md")
post_cache = post_cache_path.read_text()
post_cache = replace_once(
    post_cache,
    '''history, the source resolves the real oldest page with absolute ten-post probes, removes the phantom
logical prefix, and keeps using the corrected page arithmetic afterward.
''',
    '''history, the source resolves the exact root count with absolute `per_page=1` offset probes, removes
that phantom logical prefix, then returns to ten-post pages for visible/prefetched history.
''',
    "post-cache boundary docs",
)
post_cache_path.write_text(post_cache)
