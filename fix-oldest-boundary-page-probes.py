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
    oldestBoundaryProbeStep = 1;
    probeOldestBoundary();
}

void ChannelPostSource::probeOldestBoundary()
{
    if (!oldestBoundaryProbeInFlight) {
        return;
    }

    if (oldestBoundaryEmptyPage == 0) {
        reconcileRootCount(0);
        finishOldestBoundaryProbe();
        return;
    }

    if (oldestBoundaryNonEmptyPage >= 0
        && oldestBoundaryEmptyPage == oldestBoundaryNonEmptyPage + 1) {
        const int page = oldestBoundaryNonEmptyPage;
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
                    guard->reconcileRootCount(
                        page * ServerPageSize + static_cast<int>(result.postIds.size()));
                    guard->placePage(page, result.postIds);
                }
                guard->finishOldestBoundaryProbe();
            });
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

    // Search in normal ten-post page coordinates, but probe only the first root
    // of each candidate page. With per_page=1, page=(P * 10) addresses exactly
    // the offset at which normal page P would begin.
    const int offset = page * ServerPageSize;
    qCDebug(lcTimelineChannel).nospace()
        << "OLDEST_BOUNDARY_PROBE page=" << page
        << " offset=" << offset
        << " nonEmpty=" << oldestBoundaryNonEmptyPage
        << " empty=" << oldestBoundaryEmptyPage
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
            } else {
                guard->oldestBoundaryEmptyPage = std::min(
                    guard->oldestBoundaryEmptyPage, page);
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
source_path.write_text(source)

header_path = Path("sources/chat-area/ChannelPostSource.h")
header = header_path.read_text()
header = replace_once(
    header,
    '''    // Visible/prefetch channel ranges use Mattermost's ten-post absolute
    // pages. Boundary discovery deliberately uses per_page=1 probes so it does
    // not fetch viewport-sized payloads merely to test whether an offset exists.
''',
    '''    // Visible/prefetch channel ranges use Mattermost's ten-post absolute
    // pages. Boundary discovery probes candidate page starts with per_page=1 so
    // binary search does not fetch viewport-sized payloads.
''',
    "page-size comment",
)
header = replace_once(
    header,
    '''    int oldestBoundaryNonEmptyOffset = -1;
    int oldestBoundaryEmptyOffset = -1;
''',
    '''    int oldestBoundaryNonEmptyPage = -1;
    int oldestBoundaryEmptyPage = -1;
''',
    "boundary state",
)
header_path.write_text(header)

long_list_path = Path("docs/long-list-architecture.md")
long_list = long_list_path.read_text()
long_list = replace_once(
    long_list,
    '''range. Boundary discovery switches to `per_page=1`: in that coordinate system `page=N` asks only
whether the root at newest-offset N exists. The source probes backward from the empty ten-post page
with exponentially increasing offset steps, then binary-searches the remaining item interval. The
first empty one-post offset is the exact real root count. The phantom logical prefix is removed once,
then the actual oldest viewport block is fetched with the normal `per_page=10` policy. No identity
cursor is introduced by this reconciliation path.
''',
    '''range. Boundary search stays in ten-post page coordinates but tests each candidate page with one
root only: candidate page P is probed as `page=P*10&per_page=1`, which asks whether the first offset
of that ten-post page exists. Exponential stepping followed by binary search therefore keeps the same
request count without downloading ten posts per probe. Once the last existing page is known, that
page alone is fetched with `per_page=10`; its returned length gives the exact real root count and
materializes the oldest viewport block. The phantom logical prefix is then removed once and normal
ten-post paging continues. No identity cursor is introduced by this reconciliation path.
''',
    "long-list docs",
)
long_list_path.write_text(long_list)

post_cache_path = Path("docs/post-cache.md")
post_cache = post_cache_path.read_text()
post_cache = replace_once(
    post_cache,
    '''history, the source resolves the exact root count with absolute `per_page=1` offset probes, removes
that phantom logical prefix, then returns to ten-post pages for visible/prefetched history.
''',
    '''history, the source probes candidate ten-post page starts with `per_page=1`, loads only the resolved
oldest page with `per_page=10`, removes the phantom logical prefix, and then keeps ten-post paging for
visible/prefetched history.
''',
    "post-cache docs",
)
post_cache_path.write_text(post_cache)
