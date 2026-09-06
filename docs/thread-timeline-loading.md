# Thread timeline loading contract

This document defines the data-loading invariants for partially available chat and thread timelines.
It complements `long-list-architecture.md`; the ownership rules there remain authoritative.

## Prefetch before a logical gap

`LongListWidget` owns the decision that more logical items are needed. A post source must not inspect
scrollbar pixels or viewport geometry to implement prefetch.

For ordinary scrolling the desired logical range is the union of:

- the visible viewport;
- the configured screen/pixel buffer; and
- a hard logical margin of **5 items on each side of the visible range**.

The logical margin is a minimum, not a replacement for the screen buffer. In particular, unusually
tall posts must not reduce prefetch to one or two logical rows merely because one screen already
contains enough pixels.

The observable requirement is:

> If an unavailable logical range is adjacent in the user's scroll direction, its request must be
> issued no later than the point where fewer than 5 available logical items remain between the
> visible viewport edge and that unavailable range.

The request is still rounded to the normal request block size. Loading and materialization should
therefore normally happen outside the viewport; the user must not have to scroll into an estimated,
unavailable region to trigger its fetch.

## Thread logical-index authority

An exact-count thread uses oldest-to-newest logical indices:

```text
0                  root post
1..reply_count     replies, oldest -> newest
```

Only data with known positional provenance may claim an exact logical index.

Authoritative placements are:

- the initial page at the oldest boundary;
- the tail page at the newest boundary;
- a page fetched immediately before or after an already mapped post cursor.

A partial `BackendChannel` cache is **not** automatically an authoritative contiguous page. It may
contain a tail page, an older context window, or several disjoint windows. Unless all replies are
known, the source must not pack arbitrary cached replies into a synthetic prefix or suffix merely to
fill logical slots.

## Filling a gap

When a requested unavailable range touches an already mapped post, use that post as an authoritative
cursor before falling back to timestamp approximation:

```text
known predecessor | GAP
        -> fetch direction=down from (fromCreateAt, fromPost)

GAP | known successor
        -> fetch direction=up from (fromCreateAt, fromPost)
```

Mattermost thread pagination uses `(fromCreateAt, fromPost)` as a compound cursor. Supplying
`fromPost` without its `create_at` is invalid and must never be emitted by `PostRepository`.

Cursor responses are placed at the exact logical window implied by the cursor. They must not pass
through approximate index placement and must not relocate already authoritative overlap rows.

## Random middle seek

Timestamp-based `fromCreateAt` loading is only a seed mechanism for a genuinely disconnected random
seek where neither adjacent logical boundary is known.

After a timestamp seed overlaps or establishes an authoritative mapped row, further expansion toward
the viewport/buffer must continue with before/after post cursors. Repeating the same approximate page
against a known adjacent gap is a bug: it can leave a permanent estimated-height hole or, if known
identities are relocated to the estimate, create an identity ping-pong and visible tremor.

## Count and boundary differences from channel history

Channel history needs an absolute-page boundary repair because `total_msg_count_root` may count rows
that ordinary `/channels/{id}/posts` history does not return. Threads deliberately do not inherit that
page-probing algorithm. Mattermost `Thread.ReplyCount` excludes deleted replies, and the thread endpoint
is cursor/time based (`fromCreateAt`, `fromPost`, `direction`) rather than an absolute `page=N` space.

If a future server/plugin configuration produces a real thread count mismatch, the common abstraction
should be logical-count reconciliation only. Transport-specific boundary evidence remains in the source;
`LongListWidget` must stay unaware of Mattermost counts, pages and cursors.

## Stability requirements

A page response that does not change logical identity mapping must not cause existing visible
`PostWidget`s to be destroyed and recreated.

Newly filled empty slots require availability notification. `itemsChanged` is reserved for logical
indices whose previously concrete identity/content really changed.

All viewport position preservation, request look-ahead calculation, materialization and scrolling
remain inside `LongListWidget`. Thread-specific logical identity and cursor choice remain inside
`ThreadPostSource`; Mattermost REST transport, pagination encoding, response normalization and cache
ingestion remain inside the single `PostRepository`.