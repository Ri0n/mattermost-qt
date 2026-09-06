from pathlib import Path


def replace_once(path, old, new):
    p = Path(path)
    text = p.read_text()
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected one match, found {count}")
    p.write_text(text.replace(old, new, 1))


replace_once(
    "docs/post-cache.md",
    '''This document defines the persistent and resident post-cache contract below the chat post sources.\nIt extends the cache boundary in `long-list-architecture.md`; it does not move any geometry or\nviewport responsibility out of `LongListWidget`.\n''',
    '''This document defines the persistent and resident post-cache contract below the chat post sources.\nIt extends the cache boundary in `long-list-architecture.md`; it does not move any geometry or\nviewport responsibility out of `LongListWidget`.\n\n`post-cache-runtime.md` is the detailed execution contract for snapshot authority, account isolation,\nobservation sequences, async worker ordering, direct cache-first reads and future provisional newest\nwindow hydration. `post-source-architecture.md` defines which logical-index operations are shared by\nchannel/thread sources and which boundary proofs stay transport-specific.\n''',
)
replace_once(
    "docs/post-cache.md",
    '''Cache-first hydration is deliberately not enabled yet, but the resident refresh prerequisite is\nnow in place. `BackendChannel::mergePostContext()` refreshes an already-resident ID in place from the\naccepted full JSON snapshot, preserving the stable `BackendPost*` address used by current widgets and\nsources. `BackendPost` replaces all server-backed post fields rather than only the message, while\npreserving separately fetched poll metadata and local annotations absent from raw REST/cache JSON.\nDirect post lookup now has its first cache-read path. An asynchronous SQLite hit may insert an absent\nresident post and satisfy the caller immediately, but it never refreshes an already-resident object and\nnever advances the resident server-observation watermark. The normal HTTP request is still dispatched\nand validates/refreshes that object in the background; a failed result is delivered only after both\ncache and HTTP miss. Cached identity/timestamps still have no absolute-page authority. Newest channel\nand thread window hydration remains the next read-side step.\n''',
    '''Cache-first **window** hydration is deliberately not enabled yet, but the resident refresh prerequisite\nand direct-post read path are now in place. `BackendChannel::mergePostContext()` refreshes an\nalready-resident ID in place from the accepted full JSON snapshot, preserving the stable\n`BackendPost*` address used by current widgets and sources. `BackendPost` replaces all server-backed\npost fields rather than only the message, while preserving separately fetched poll metadata and local\nannotations absent from raw REST/cache JSON.\n\nDirect `loadPost()` is cache-first: an asynchronous SQLite hit may insert an absent resident post and\nsatisfy the caller immediately, but it never refreshes an already-resident object and never advances\nthe resident server-observation watermark. The normal HTTP request is still dispatched and\nvalidates/refreshes that object in the background; a failed result is delivered only after both cache\nand HTTP miss. Cached identity/timestamps still have no absolute-page authority. Newest channel and\nthread window hydration remains the next read-side step and must stay provisional until endpoint-specific\nHTTP boundary evidence arrives.\n''',
)

replace_once(
    "docs/long-list-architecture.md",
    '''   ChatLogWidget                   Mattermost post UI + semantic post identity\n        |\n        +-------------------+\n        |                   |\n ChannelPostSource      ThreadPostSource\n        |                   |\n        +---------+---------+\n                  |\n          PostTimelineService\n''',
    '''   ChatLogWidget                   Mattermost post UI + semantic post identity\n        |\n        v\n AbstractPostSource              source/view contract only\n        |\n        v\n  IndexedPostSource              shared logical ID slots + structural signals\n        |\n        +-------------------+\n        |                   |\n ChannelPostSource      ThreadPostSource\n        |                   |\n        +---------+---------+\n                  |\n          PostTimelineService\n''',
)
replace_once(
    "docs/long-list-architecture.md",
    '''`LongListWidget` owns geometry, scrolling, viewport anchoring and persistent logical-item viewport\nlocks. `ChatLogWidget` owns post-specific presentation, actions and semantic post-ID identity. A post\nsource owns logical-index-to-post identity and range availability. `PostTimelineService` owns range\nretrieval, in-flight request coalescing and cache tiers.\n''',
    '''`LongListWidget` owns geometry, scrolling, viewport anchoring and persistent logical-item viewport\nlocks. `ChatLogWidget` owns post-specific presentation, actions and semantic post-ID identity.\n`AbstractPostSource` is the view/source interface; `IndexedPostSource` owns the transport-agnostic\nlogical ID slot map, exact-window mutation and structural source signals shared by channel/thread\nsources. Concrete sources alone decide what server evidence makes a placement exact.\n`PostTimelineService` owns range retrieval, in-flight request coalescing and cache tiers. See\n`post-source-architecture.md` for the complete source-layer contract.\n''',
)
replace_once(
    "docs/long-list-architecture.md",
    '''`ChannelPostSource` and `ThreadPostSource` adapt different Mattermost endpoints into the same logical\ncontract. They do not manipulate widgets or scrollbars.\n''',
    '''`ChannelPostSource` and `ThreadPostSource` adapt different Mattermost endpoints into the same logical\ncontract. Their common index-to-ID bookkeeping lives in `IndexedPostSource`; absolute channel pages,\nchannel count repair, thread root/cursor semantics and endpoint-specific boundary proofs remain in the\nconcrete source. They do not manipulate widgets or scrollbars.\n''',
)

replace_once(
    "docs/thread-timeline-loading.md",
    '''This document defines the data-loading invariants for partially available chat and thread timelines.\nIt complements `long-list-architecture.md`; the ownership rules there remain authoritative.\n''',
    '''This document defines the data-loading invariants for partially available chat and thread timelines.\nIt complements `long-list-architecture.md`; the ownership rules there remain authoritative. Shared\nlogical ID-slot bookkeeping is documented in `post-source-architecture.md`; this document covers only\nthread-specific topology and transport authority.\n''',
)
replace_once(
    "docs/thread-timeline-loading.md",
    '''If a future server/plugin configuration produces a real thread count mismatch, the common abstraction\nshould be logical-count reconciliation only. Transport-specific boundary evidence remains in the source;\n`LongListWidget` must stay unaware of Mattermost counts, pages and cursors.\n''',
    '''If a future server/plugin configuration produces a real thread count mismatch, only the structural\nslot mutation belongs in the shared `IndexedPostSource` layer. The proof that a particular count is\ncorrect remains thread-specific, just as `/posts` page-boundary proof remains channel-specific.\n`LongListWidget` must stay unaware of Mattermost counts, pages and cursors.\n''',
)

print("architecture docs linked")
