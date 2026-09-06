# Post cache architecture

This document defines the persistent and resident post-cache contract below the chat post sources.
It extends the cache boundary in `long-list-architecture.md`; it does not move any geometry or
viewport responsibility out of `LongListWidget`.

## Goals

The cache exists to make already-seen history cheap to revisit and cheap to restore after restart,
without turning the in-process `BackendChannel` model into an unbounded second copy of server
history.

The data path is:

```text
ChannelPostSource / ThreadPostSource
                |
                v
        PostTimelineService
       (PostRepository today)
                |
        +-------+-------+
        |               |
 resident posts    PostCacheService
                        |
                 dedicated QThread
                        |
                  PostCacheStore
                        |
                      SQLite
        |               |
        +-------+-------+
                |
              HTTP
```

The current `PostTimelineService` name is still a compatibility alias for `PostRepository`. The
cache is introduced at that boundary first; the mechanical rename can happen separately without
changing cache semantics.

## Channel admission policy

Mattermost can deliver live post events for many joined channels even when the user rarely opens
most of them. Message activity therefore must **not** be treated as cache interest: otherwise a busy
channel can keep itself permanently hot in both RAM and SQLite without any reading gesture from the
user.

Cache interest is driven only by the most recent **channel open** observation:

- resident-memory admission horizon: **1 hour** by default;
- persistent-disk admission horizon: **10 hours** by default, approximately one working day;
- opening a thread counts as opening its parent channel;
- notification/permalink/Attention navigation counts once it actually opens the channel;
- incoming posts, mentions, unread state, typing and reactions never refresh cache interest;
- the currently open channel is always hot because activation records a fresh open observation.

The existing Mattermost `channel_open_time` preference is a useful startup seed. It lets the client
recover recent channel interest across process restarts. A local `ChatArea` activation updates the
same policy immediately rather than waiting for a preference round trip. `channel_open_time` is
preferred over `last_post_at`, `last_viewed_at` or generic activity timestamps because those can move
without a deliberate local reading gesture.

The two horizons are **admission and retention gates**, not replacements for LRU/TTL limits:

```text
channel opened <= 1 h ago
        -> eligible for resident materialization
channel opened <= 10 h ago
        -> eligible for durable post storage
older / never opened
        -> metadata/unread processing only; no post-body cache admission
```

A channel crossing the one-hour memory horizon makes all of its unleased resident posts immediately
eligible for eviction even if the channel is busy. A channel crossing the ten-hour disk horizon is
removed from persistent post storage during cache maintenance. Global byte/count LRU remains a
secondary pressure rule among still-eligible channels.

Persistent channel-interest metadata belongs in the cache database so expiry can be enforced by the
worker without depending on UI lifetime:

```text
channel_usage
    account_id
    channel_id
    last_opened_at

PRIMARY KEY(account_id, channel_id) WITHOUT ROWID
INDEX(account_id, last_opened_at)
```

Like posts themselves, channel usage is account-scoped. A visit from account A must never admit rows
for account B.

## Persistent store

All accounts use one SQLite database under `QStandardPaths::CacheLocation`. Rows are isolated by a
small integer `account_id` derived from `(server, Mattermost user id)` so secondary indexes do not
repeat a long server URL in every entry.

Post payloads remain Mattermost JSON rather than a duplicated normalized object schema. The payload
is encoded with `QJsonDocument::Compact` and compressed before storage. Only fields required for
lookup, ordering and eviction are indexed separately:

```text
accounts
    id INTEGER PRIMARY KEY
    server TEXT
    user_id TEXT

posts
    account_id
    post_id
    channel_id
    root_id
    create_at
    update_at
    last_access
    payload BLOB

PRIMARY KEY(account_id, post_id) WITHOUT ROWID
INDEX(account_id, channel_id, root_id, create_at, post_id)
INDEX(last_access)

channel_usage
    account_id
    channel_id
    last_opened_at

PRIMARY KEY(account_id, channel_id) WITHOUT ROWID
INDEX(account_id, last_opened_at)
```

`root_id == ''` identifies channel roots. A non-empty `root_id` identifies replies in that thread.
Attachments themselves are not stored here; only the post JSON and attachment metadata already
contained in it are cached.

The cache is intentionally not encrypted at rest in this first implementation. Account scoping
prevents accidental cross-account reuse inside the client, but it is not filesystem encryption.

## SQLite thread ownership and asynchronous writes

SQLite work must never block the UI/network callback thread. `PostCacheStore` is deliberately a
synchronous, thread-confined object; `PostCacheService` is the asynchronous boundary.

`PostCacheService` owns one dedicated `QThread`. The worker lazily constructs `PostCacheStore` from
inside that thread, so the `QSqlDatabase` connection and the store's periodic maintenance timer are
created, used and destroyed on the same thread. `PostCacheStore` is never constructed on one thread
and then moved with a live SQL connection.

Every queued operation carries its complete account key:

```text
(server, Mattermost user id, operation payload)
```

The worker selects that account immediately before executing the operation. There is intentionally
no mutable "current account" on the calling thread: a queued write produced for account A must still
go to account A even if the UI logs out and selects account B before SQLite processes it.

For REST requests the account key is captured when the repository starts the logical request, not
when the response callback eventually runs. For WebSocket events it is captured when the event is
handled. This prevents an old delayed response from being filed under a newly selected account.

Writes are **write-behind physically, write-through logically**:

```text
successful server payload
        |
queue durable cache command
        |
update resident model / deliver callback
```

The UI does not wait for an SQLite commit because the cache is disposable and never server
authority. A process crash between queueing and commit may lose cache warming, but cannot lose user
or server state. Normal `PostCacheService` destruction drains all earlier queued commands, runs a
final maintenance pass and destroys the SQL connection on the worker thread before joining it.

## Causal ordering of cache writes

A response is not necessarily fresh merely because it arrives late. For example:

```text
HTTP request A starts
        |
WebSocket delete/reaction is observed
        |
cache row is invalidated
        |
old HTTP response A arrives
```

The final response must not resurrect a snapshot that predates the WebSocket mutation.
`PostRepository` therefore assigns a monotonic observation sequence per backend. The sequence is
captured by the **physical HTTP request** when it is dispatched and by each WebSocket cache
mutation when it is observed. Request coalescing shares that original request sequence; a later
caller joining an existing request cannot make the in-flight response appear newer.

The cache worker keeps a short-lived `(account, post_id) -> invalidation sequence` watermark. A
queued store whose source observation is older than or equal to a newer invalidation is discarded
for that post. Invalidations themselves are never suppressed by channel-admission policy because a
known-stale row must be removable even after its channel has gone cold.

The same ordering concept is also applied to resident refresh. Each physical HTTP request keeps
its dispatch observation sequence, while WebSocket new/edit/delete/reaction observations advance the
resident watermark immediately. `PostRepository` drops an older response per post before it reaches
`BackendChannel`, so stale REST work cannot overwrite a newer WebSocket state in memory. Reply
observations conservatively fence their root ID too because ingesting a reply can update transient
thread metadata on the root. These watermarks are short-lived in-flight causality guards, not
persistent cache freshness metadata.

## Disk limits and compaction

Persistent limits are global across the SQLite file unless stated otherwise:

- only channels opened within **10 hours** are eligible by default;
- at most **10,000 posts** total;
- at most **1,000 cached replies per thread**;
- at most **5 GiB** of compressed post payloads;
- LRU eviction by `last_access` among still-eligible rows;
- 5% hysteresis after crossing a global limit to avoid one-row eviction churn.

Row limits are enforced during writes, not only at application shutdown.

SQLite uses:

```text
page_size=4096
journal_mode=WAL
synchronous=NORMAL
auto_vacuum=INCREMENTAL
```

A very-coarse maintenance timer runs every ten minutes on the cache worker thread even when the
cache receives no lookups. A maintenance pass:

1. removes post rows for channels outside the configured disk-interest horizon;
2. re-enforces per-thread and global LRU limits;
3. executes `PRAGMA optimize`;
4. checkpoints and truncates WAL;
5. runs bounded incremental vacuum when at least 128 pages and 5% of the database are free.

Incremental vacuum is deliberate. Full `VACUUM` can temporarily require another database-sized file
and can block for a long time on a multi-gigabyte cache. Reclaiming at most 4096 pages per pass keeps
the single SQLite file compact without creating a second ~5 GiB working copy.

## Cache authority

A cached post identity/payload is useful data, but cached timestamps are **not** sufficient evidence
for an absolute Mattermost page number. New posts can shift all absolute page boundaries.

Therefore:

- direct post lookup may eventually be satisfied from SQLite immediately;
- a bounded newest suffix may eventually seed a channel/thread resident model;
- HTTP remains authoritative for absolute channel page placement and for proving oldest/newest
  boundaries;
- stale successful work may populate SQLite only when the channel remains cache-eligible, but it
  never gains viewport authority by itself.

This preserves the provisional/authoritative rules in `long-list-architecture.md`.

Cache-first hydration is deliberately not enabled yet, but the resident refresh prerequisite is
now in place. `BackendChannel::mergePostContext()` refreshes an already-resident ID in place from the
accepted full JSON snapshot, preserving the stable `BackendPost*` address used by current widgets and
sources. `BackendPost` replaces all server-backed post fields rather than only the message, while
preserving separately fetched poll metadata and local annotations absent from raw REST/cache JSON.
The next read-side step is to serve direct/newest-window cache hits as provisional resident data and
validate them with newer HTTP observations before granting any absolute-page authority.

## Write-through and invalidation

The write side of service integration is active.

Successful post-bearing REST responses handled by `PostRepository` queue full `posts` objects into
SQLite before resident ingestion only for channels admitted by the disk-interest policy. This covers
direct post retrieval, channel pages, channel cursor windows and thread windows.

WebSocket handling follows the same durable rules:

- new post: upsert the event's full post object only if its channel is disk-eligible;
- edited post: upsert the event's full post object only if its channel is disk-eligible;
- deleted post: remove the cached row regardless of eligibility;
- reaction added/removed: invalidate the cached row regardless of eligibility because these events
  do not contain a lossless full replacement post object.

Deleting/invalidation is preferable to keeping a known-stale reaction/deletion snapshot. A later
REST fetch can repopulate that row only if the channel is still eligible.

## Resident-memory policy

The persistent cache makes a large resident history unnecessary. The target resident policy is:

- only channels opened within **1 hour** are eligible for resident post bodies;
- **500 MiB hard accounted maximum** across all materialized post models;
- trim back to roughly **400 MiB** after crossing the hard limit to avoid immediate churn;
- cold-post idle TTL: **5 minutes** by default inside still-eligible channels;
- active sweep every **30 seconds**, independent of cache lookups;
- LRU is a secondary pressure rule after channel eligibility and TTL;
- posts currently pinned by a materialized widget, active edit/reply context, active thread root or
  another explicit lease are not evicted until their lease is released.

The one-hour horizon is based on channel-open time, not last message activity. WebSocket traffic for
a cold channel may still update unread/mention/notification metadata, but the event's full post body
must not become a durable `BackendPost` merely because the server delivered it. Reconnect recovery
must likewise avoid fetching/materializing post pages for every joined channel.

The 500 MiB value is cache-accounted memory, not process RSS: portable C++ cannot reliably attribute
allocator arenas and Qt internals to one cache. Each resident post will carry an estimated retained
cost based on its compact JSON size plus materialized dynamic data. The estimate should be biased
upward so the cache trims before real heap usage becomes problematic.

A resident sweep must run from its own timer. It must not depend on a future lookup to discover that
old entries expired.

### Memory compaction

Resident eviction must actually erase the owning `BackendPost` object and its identity-map entry;
clearing only widgets is not a memory cache eviction. After bulk eviction, auxiliary vectors/maps
that retain excess capacity should be rebuilt or `squeeze()`d where applicable. `std::list` nodes
are released immediately; global allocator RSS returning to the OS remains allocator/platform
specific and is not a correctness invariant.

Before true resident eviction is enabled, raw pointer lifetime has to be made explicit. In
particular:

- `BackendPost::rootPost` cannot be a durable owning/reference mechanism; `root_id` is authoritative;
- a visible `PostWidget` must hold a lease preventing its backing post from disappearing;
- sources keep post IDs, not permanent raw pointers;
- an evicted ID must be rematerializable from SQLite or HTTP without changing its logical identity.

This pointer/lease step is required before enforcing the 500 MiB resident cap. Adding an unsafe
`erase()` to the current `BackendChannel::posts` list would introduce use-after-free bugs.

## Settings surface

Cache policy is user-configurable from a dedicated **Cache** tab rather than being mixed into the
attachment/download form. The user-facing knobs are:

### Attachment files

- maximum attachment-file disk cache size.

### Post cache on disk

- channel-open horizon, default 10 hours;
- maximum compressed payload, default 5 GiB;
- maximum total posts, default 10,000;
- maximum replies per thread, default 1,000;
- maintenance interval, default 10 minutes.

### Post cache in memory

- channel-open horizon, default 60 minutes;
- hard accounted limit, default 500 MiB;
- trim target after pressure, default 400 MiB;
- cold-post TTL, default 5 minutes;
- sweep interval, default 30 seconds.

SQLite page size, journal mode, vacuum free-page threshold and bounded vacuum chunk size remain
implementation invariants. Exposing them as normal user settings would make it easy to create cache
layouts that violate the storage assumptions without providing meaningful product-level control.

## Implementation phases

### Phase 1 — durable store

Implemented in this PR:

- `PostCacheStore` and SQLite schema;
- compressed raw JSON;
- account isolation;
- LRU limits and timed incremental vacuum;
- unit tests for round-trip, isolation, channel/thread selection and eviction.

### Phase 2 — service integration and admission

Implemented/in progress in this PR:

- dedicated asynchronous `PostCacheService` / SQLite worker thread;
- capture account identity with every queued operation;
- causal observation fencing for stale HTTP versus newer WebSocket invalidations;
- write successful HTTP post objects through to SQLite;
- WebSocket new/edit write-through and delete/reaction invalidation;
- channel-open admission policy: one hour for memory, ten hours for disk;
- seed channel-open time from Mattermost preferences and refresh it on local activation;
- dedicated Cache settings tab for all user-facing limits.

Implemented prerequisites for reads:

- full in-place refresh semantics when fresh JSON arrives for an already resident `BackendPost`;
- resident causal fencing against stale HTTP responses.

Still required to enable reads:

- direct `loadPost()` cache hit followed by background validation;
- seed a small newest channel/thread window from SQLite before normal server range fetch.

### Phase 3 — bounded resident cache

- replace durable raw-pointer assumptions with explicit resident leases/ID resolution;
- separate transient WebSocket notification/unread processing from durable post materialization;
- stop reconnect post-page materialization for channels outside the memory horizon;
- add per-post memory-cost accounting;
- 30-second sweeper, 5-minute cold TTL, one-hour channel horizon, 500 MiB hard / ~400 MiB target;
- remove cold `BackendPost` objects and compact auxiliary containers;
- rematerialize on source demand from SQLite, then HTTP.

### Phase 4 — restart validation and tuning

- restore eligible cached newest suffixes immediately on channel/thread open;
- validate in the background without moving the viewport;
- expose cache statistics/logging for tuning limits and vacuum thresholds.


## Timeline authority and reconnect validation

Persistent caching must not make Mattermost's approximate message counts an
authority for post identity. `total_msg_count_root` is useful for scrollbar scale and for choosing an initial random-seek
page, but it is not `/posts` row count: deleted roots can make it too large, while join/leave and
other count-excluded system roots can make it too small. Concurrent server changes can add another
source of disagreement.

The channel source keeps two separate concepts:

1. ordinary channel range loading uses absolute `/posts?page=N&per_page=10` pages only;
2. known post identities and timestamps help estimate semantic targets and reconcile overlap, but
   are not promoted into `before=<post_id>` / `after=<post_id>` boundaries for ordinary scrolling.

The ten-post page size is invariant. Logical request blocks are oldest-aligned while Mattermost
pages are newest-aligned, so a ten-item logical block can cross a server-page boundary. In that
case the source requests both intersecting pages and places each with `placePage()`. Remote thumb
seek, normal scrolling and initial tail materialization therefore share exactly the same paging
path instead of switching between page arithmetic and cursor walks. A successful empty absolute
page is also authoritative boundary evidence. `total_msg_count_root` can overstate `/posts` when
deleted roots disappear, but it can also understate it because join/leave and other count-excluded
system roots remain visible in channel history. A large top-edge request starts with a one-root probe
3% inside the estimate. Empty results search inward; existing data searches outward to the estimate
and, when the reported oldest page is full, continues beyond it until `/posts` proves the real edge.
Small estimates use a normal ten-post page first and expand outward if that page disproves the count.
Exact reconciliation removes or inserts an oldest logical prefix while preserving newest-anchored page
mapping. The 3% value is a latency heuristic, never a correctness assumption.

Every successful range request ends in
one of three states: new identities were placed, a real boundary removed stale
logical slots, or the request made no progress and is finished without an
immediate retry loop.

WebSocket reconnect uses the same bounded-working-set rule. A successful
Mattermost sequence resume requires no HTTP history replay. If reliable replay
explicitly fails, only the currently viewed conversation is validated
immediately; inactive joined channels are left lazy and are validated when
opened. This prevents a reconnect from filling either the network queue or the
post cache with channels the user is not reading.

Cache admission follows user interest rather than membership. By default a
channel is eligible for resident-memory post caching for one hour after it was
viewed, and for persistent SQLite post caching for ten hours after it was
viewed. The current channel is always considered interested. These intervals,
memory/disk limits, maintenance cadence, TTLs and vacuum controls are exposed
on the cache settings page and are policy inputs rather than timeline geometry.
