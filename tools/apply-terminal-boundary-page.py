from pathlib import Path

cpp = Path("sources/chat-area/ChannelPostSource.cpp")
text = cpp.read_text()
old = '''    // If the 3% heuristic is only one page, materializing the candidate block
    // is cheaper than maintaining a separate probe phase. Once binary search
    // leaves at most one unknown page, materialize the known non-empty page
    // first: if it is short, the boundary is proved without probing that last
    // unknown page. A full page then needs at most the one adjacent page.
    if ((oldestBoundaryNonEmptyPage < 0 && oldestBoundaryProbeStep == 1)
        || (oldestBoundaryNonEmptyPage >= 0 && oldestBoundaryEmptyPage >= 0
            && oldestBoundaryEmptyPage - oldestBoundaryNonEmptyPage <= 2)) {
        if (oldestBoundaryNonEmptyPage < 0) {
            oldestBoundaryProbeStep = 2;
            loadOldestBoundaryPage(std::max(0, oldestBoundaryEmptyPage - 1));
        } else {
            loadOldestBoundaryPage(oldestBoundaryNonEmptyPage);
        }
        return;
    }
'''
new = '''    // If the 3% heuristic is only one page, materializing the candidate block
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
'''
if old not in text:
    raise SystemExit("ChannelPostSource terminal-boundary block not found")
cpp.write_text(text.replace(old, new, 1))

doc = Path("docs/long-list-architecture.md")
text = doc.read_text()
old = '''Near the end of binary search, when at most one unknown ten-post page remains, the source fetches the
page immediately before the known empty boundary with `per_page=10`. A short result proves the exact
oldest boundary immediately; a full result adjacent to the empty page proves an exact multiple of ten;
and an empty result moves the boundary one page inward. In every case any returned posts are already
useful viewport/prefetch materialization. The phantom logical prefix is removed once the exact count is
known and normal ten-post paging continues. No identity cursor is introduced by this reconciliation path.
'''
new = '''Near the end of binary search, when at most two unknown ten-post pages remain, the source stops
spending one-root probes on that tiny interval and fetches the first unknown page with `per_page=10`.
A short result proves the exact oldest boundary immediately; an empty result tightens the boundary; and
a full page requires at most one adjacent ten-post page to finish. In every case returned posts are
already useful viewport/prefetch materialization. The phantom logical prefix is removed once the exact
count is known and normal ten-post paging continues. No identity cursor is introduced by this
reconciliation path.
'''
if old not in text:
    raise SystemExit("long-list boundary paragraph not found")
doc.write_text(text.replace(old, new, 1))
