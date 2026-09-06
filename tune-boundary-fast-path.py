from pathlib import Path
import re


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one match, got {count}")
    return text.replace(old, new, 1)


header = Path("sources/chat-area/ChannelPostSource.h")
text = header.read_text()
text = replace_once(
    text,
    """    void resolveOldestBoundary(int emptyPage, std::function<void()> completion);
    void probeOldestBoundary();
    void loadOldestBoundaryPage(int page);
""",
    """    void probeEstimatedOldestBoundary(std::function<void()> completion);
    void resolveOldestBoundary(int emptyPage, std::function<void()> completion);
    void probeOldestBoundary();
    void loadOldestBoundaryPage(int page);
""",
    "header method",
)
text = replace_once(
    text,
    """    bool oldestBoundaryProbeInFlight = false;
    int oldestBoundaryNonEmptyPage = -1;
    int oldestBoundaryEmptyPage = -1;
    int oldestBoundaryProbeStep = 1;
    int oldestBoundaryFullPageChecked = -1;
""",
    """    bool oldestBoundaryFastPathTried = false;
    bool oldestBoundaryProbeInFlight = false;
    int oldestBoundaryNonEmptyPage = -1;
    int oldestBoundaryEmptyPage = -1;
    int oldestBoundaryProbeStep = 1;
""",
    "header state",
)
header.write_text(text)

cpp = Path("sources/chat-area/ChannelPostSource.cpp")
text = cpp.read_text()

anchor = """    if (requestedLast < requestedFirst) {
        emit rangeRequestFinished(first, last);
        return;
    }

    int firstMissing = requestedFirst;
"""
replacement = """    if (requestedLast < requestedFirst) {
        emit rangeRequestFinished(first, last);
        return;
    }

    // A large channel is unlikely to have an exact oldest page when the server
    // count still includes deleted roots. Avoid downloading one or two guessed
    // ten-post pages just to discover that they are empty: validate the
    // estimated oldest page with one root first, then jump inward by the 3%
    // heuristic if that cheap probe is empty. Small channels keep the normal
    // ten-post path because their likely oldest block is cheap and useful.
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
"""
text = replace_once(text, anchor, replacement, "requestRange fast path")

pattern = re.compile(
    r"void ChannelPostSource::resolveOldestBoundary\(int emptyPage,\n"
    r"\s+std::function<void\(\)> completion\)\n\{.*?\n\}\n\n"
    r"void ChannelPostSource::finishOldestBoundaryProbe\(\)\n\{.*?\n\}\n",
    re.S,
)
new_block = r'''void ChannelPostSource::probeEstimatedOldestBoundary(
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

    const int page = pageForIndex(0);
    const int offset = page * ServerPageSize;
    qCDebug(lcTimelineChannel).nospace()
        << "OLDEST_BOUNDARY_INITIAL page=" << page
        << " offset=" << offset
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
                // The estimate reached real data, so turn the cheap validation
                // into useful materialization. A short page proves the exact
                // oldest boundary; a full page is still the best first block for
                // the requested top viewport.
                guard->oldestBoundaryNonEmptyPage = page;
                guard->loadOldestBoundaryPage(page);
                return;
            }

            guard->oldestBoundaryEmptyPage = page;
            guard->probeOldestBoundary();
        });
}

void ChannelPostSource::resolveOldestBoundary(int emptyPage,
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

    // If the 3% heuristic is only one page, materializing the candidate block
    // is cheaper than maintaining a separate probe phase. Likewise, once binary
    // search leaves at most one unknown page, fetch the page immediately before
    // the known empty boundary. It either proves the boundary or shrinks it by
    // one page, and the payload is useful to the top viewport/prefetch window.
    if ((oldestBoundaryNonEmptyPage < 0 && oldestBoundaryProbeStep == 1)
        || (oldestBoundaryNonEmptyPage >= 0
            && oldestBoundaryEmptyPage - oldestBoundaryNonEmptyPage <= 2)) {
        if (oldestBoundaryNonEmptyPage < 0) {
            oldestBoundaryProbeStep = 2;
        }
        loadOldestBoundaryPage(std::max(0, oldestBoundaryEmptyPage - 1));
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
            } else {
                guard->oldestBoundaryEmptyPage = std::min(
                    guard->oldestBoundaryEmptyPage, page);
            }
            guard->probeOldestBoundary();
        });
}

void ChannelPostSource::loadOldestBoundaryPage(int page)
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
                }
                guard->probeOldestBoundary();
                return;
            }

            guard->oldestBoundaryNonEmptyPage = std::max(
                guard->oldestBoundaryNonEmptyPage, page);
            const int returned = static_cast<int>(result.postIds.size());
            if (returned < ServerPageSize) {
                guard->reconcileRootCount(page * ServerPageSize + returned);
                guard->placePage(page, result.postIds);
                guard->finishOldestBoundaryProbe();
                return;
            }

            guard->placePage(page, result.postIds);
            if (guard->oldestBoundaryEmptyPage == page + 1) {
                guard->reconcileRootCount((page + 1) * ServerPageSize);
                guard->finishOldestBoundaryProbe();
                return;
            }

            // A proactive top check can reach a full estimated oldest page
            // without having an independently proven empty page after it. The
            // block is still useful materialization; avoid inventing boundary
            // evidence from the approximate count itself.
            if (guard->oldestBoundaryEmptyPage < 0) {
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

    for (auto& waiter : waiters) {
        if (waiter) {
            waiter();
        }
    }
}
'''
text, count = pattern.subn(new_block, text, count=1)
if count != 1:
    raise SystemExit(f"boundary block: expected one match, got {count}")
cpp.write_text(text)

doc = Path("docs/long-list-architecture.md")
text = doc.read_text()
old = """The first exponential step is a heuristic 3% of the estimated root count, rounded to ten-post pages.
This ratio affects latency only; correctness does not depend on it. If 3% is no larger than one page,
the immediately preceding page is fetched with `per_page=10` instead, because a small channel is cheap
to materialize and that request is likely to be the actual oldest block. If the 3% probe is still empty,
the step grows exponentially until data brackets the boundary, then binary search refines it.

Near the end of binary search, when only one unknown ten-post page remains between known data and
known emptiness, the source first fetches the known non-empty page with `per_page=10`. A short result
proves the exact oldest boundary immediately and is already useful viewport materialization; a full
result remains useful prefetch and the one-post search continues. The phantom logical prefix is removed
once the exact count is known and normal ten-post paging continues. No identity cursor is introduced by
this reconciliation path.
"""
new = """For a large top-edge request the source does not first download guessed ten-post pages. It validates
the estimated oldest page with `per_page=1`; if that page is empty, the first inward step is a heuristic
3% of the estimated root count, rounded to ten-post pages. This ratio affects latency only; correctness
does not depend on it. If the 3% distance is no larger than one page, the normal ten-post path is used
instead because a small channel is cheap to materialize. If the 3% probe is still empty, the step grows
exponentially until data brackets the boundary, then binary search refines it.

Near the end of binary search, when at most one unknown ten-post page remains, the source fetches the
page immediately before the known empty boundary with `per_page=10`. A short result proves the exact
oldest boundary immediately; a full result adjacent to the empty page proves an exact multiple of ten;
and an empty result moves the boundary one page inward. In every case any returned posts are already
useful viewport/prefetch materialization. The phantom logical prefix is removed once the exact count is
known and normal ten-post paging continues. No identity cursor is introduced by this reconciliation path.
"""
text = replace_once(text, old, new, "long-list docs")
doc.write_text(text)

doc = Path("docs/post-cache.md")
text = doc.read_text()
old = """page is also authoritative boundary evidence: when `total_msg_count_root` overstates `/posts`
history, the source starts distant boundary probing at a heuristic 3% of the estimated root count,
probes candidate ten-post page starts with `per_page=1`, opportunistically materializes a useful full
ten-post block when the search becomes local, removes the phantom logical prefix, and then keeps
ten-post paging for visible/prefetched history. The 3% value is a latency heuristic, never a correctness
assumption.
"""
new = """page is also authoritative boundary evidence: when `total_msg_count_root` may overstate `/posts`
history, a large top-edge request first validates the estimated oldest page with `per_page=1` instead
of downloading guessed ten-post pages. An empty validation starts distant probing at a heuristic 3% of
the estimated root count; candidate page starts use `per_page=1` until the search becomes local, where
the page immediately before known emptiness is materialized with ten posts. The source then removes any
phantom logical prefix and keeps ten-post paging for visible/prefetched history. The 3% value is a latency
heuristic, never a correctness assumption.
"""
text = replace_once(text, old, new, "post-cache docs")
doc.write_text(text)
