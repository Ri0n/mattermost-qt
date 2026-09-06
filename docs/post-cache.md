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

## Disk limits and compaction

Persistent limits are global across the SQLite file unless stated otherwise:

- at most **10,000 posts** total;
- at most **1,000 cached replies per thread**;
- at most **5 GiB** of compressed post payloads;
- LRU eviction by `last_access`;
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

1. re-enforces per-thread and global LRU limits;
2. executes `PRAGMA optimize`;
3. checkpoints and truncates WAL;
4. runs bounded incremental vacuum when at least 128 pages and 5% of the database are free.

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
- stale successful work may populate SQLite but never gains viewport authority by itself.

This preserves the provisional/authoritative rules in `long-list-architecture.md`.

Cache-first hydration is deliberately not enabled yet. `BackendChannel::mergePostContext()` currently
skips a post ID that is already resident, and `BackendPost::updatePostEdits()` does not perform a full
raw-JSON refresh. Hydrating an older SQLite snapshot first would therefore allow a later fresh HTTP
snapshot with the same ID to be ignored. Full existing-post refresh semantics are a prerequisite for
using SQLite as a read tier.

## Write-through and invalidation

The write side of service integration is active.

All successful post-bearing REST responses handled by `PostRepository` queue their full `posts`
objects into SQLite before resident ingestion. This covers direct post retrieval, channel pages,
channel cursor windows and thread windows. A successful response can still warm the durable cache if
the requesting `BackendChannel` has disappeared before the response arrives.

WebSocket handling uses the same durable rules and queues the cache operation before looking up the
resident channel:

- new post: upsert the event's full post object;
- edited post: upsert the event's full post object;
- deleted post: remove the cached row;
- reaction added/removed: invalidate the cached row because these events do not contain a lossless
  full replacement post object.

Deleting/invalidation is preferable to keeping a known-stale reaction/deletion snapshot. A later
REST fetch can repopulate that row with authoritative raw JSON.

## Resident-memory policy

The persistent cache makes a large resident history unnecessary. The target resident policy is:

- **500 MiB hard accounted maximum** across all materialized post models;
- trim back to roughly **400 MiB** after crossing the hard limit to avoid immediate churn;
- cold-post idle TTL: **5 minutes** by default;
- active sweep every **30 seconds**, independent of cache lookups;
- LRU is a secondary pressure rule after TTL;
- posts currently pinned by a materialized widget, active edit/reply context, active thread root or
  another explicit lease are not evicted until their lease is released.

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

## Implementation phases

### Phase 1 — durable store

Implemented in this PR:

- `PostCacheStore` and SQLite schema;
- compressed raw JSON;
- account isolation;
- LRU limits and timed incremental vacuum;
- unit tests for round-trip, isolation, channel/thread selection and eviction.

### Phase 2 — service integration

Implemented write side:

- dedicated asynchronous `PostCacheService` / SQLite worker thread;
- capture account identity with every queued operation;
- write all HTTP-ingested post objects through to SQLite;
- WebSocket new/edit write-through;
- WebSocket delete/reaction invalidation.

Still required before enabling reads:

- full refresh semantics when fresh JSON arrives for an already resident `BackendPost`;
- direct `loadPost()` cache hit followed by background validation;
- seed a small newest channel/thread window from SQLite before normal server range fetch.

### Phase 3 — bounded resident cache

- replace durable raw-pointer assumptions with explicit resident leases/ID resolution;
- add per-post memory-cost accounting;
- 30-second TTL sweeper, 5-minute cold TTL, 500 MiB hard / ~400 MiB target;
- remove cold `BackendPost` objects and compact auxiliary containers;
- rematerialize on source demand from SQLite, then HTTP.

### Phase 4 — restart validation and tuning

- restore cached newest suffixes immediately on channel/thread open;
- validate in the background without moving the viewport;
- expose cache statistics/logging for tuning limits and vacuum thresholds.
