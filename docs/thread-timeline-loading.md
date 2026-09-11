# Thread timeline loading contract

This document defines the data-loading invariants for partially available chat and thread timelines.
It complements `long-list-architecture.md`; the ownership rules there remain authoritative. Shared
logical ID-slot bookkeeping and source-side request attachment are documented in
`post-source-architecture.md`; this document covers only thread-specific topology and transport
authority.

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

For ordinary viewport work `LongListWidget` reports the contiguous missing logical demand instead of
splitting it into transport-sized ten-item blocks. If the desired tail is `155..168`, the source sees
that whole range and can satisfy it with one edge request. Transport minimums/page sizes remain a
source/repository concern.

Random seek is different: a genuinely disconnected thumb jump still uses a small seed window around
the target so a huge estimated viewport range does not become one giant speculative HTTP request.

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

## Edge bootstrap and ordinary scrolling

The two thread edges are intrinsically useful anchors and should be used directly:

```text
requested range starts at root
    -> one oldest/initial request sized for the demand

requested range reaches logical tail
    -> one newest/tail request sized for the demand
```

This is intentionally different from converting each ten-item logical block into an independent HTTP
operation. A Full-HD viewport may need roughly a dozen rows and a larger correctly scaled display may
need a few dozen; the source should issue one edge bootstrap and let real row heights determine whether
another cursor fetch is needed afterwards.

When the first response makes a real adjacent identity available, further ordinary scrolling switches
to exact compound cursors rather than re-bootstrapping the edge.

## Filling a gap

When a requested unavailable range touches an already mapped post, use that post as an authoritative
cursor before falling back to timestamp approximation:

```text
known predecessor | GAP
        -> fetch direction=down from (fromCreateAt, fromPost)

GAP | known successor
        -> fetch direction=up from (fromCreateAt, fromPost)
```

A requested range may contain more than one unavailable run after several random seeks have created
disjoint authoritative islands. Those runs are independent gaps: `ThreadPostSource` must select one
contiguous missing run at a time and use an adjacent authoritative island as its cursor. Treating the
first and last unavailable indices across an intervening known island as one synthetic gap hides a
perfectly usable cursor and can incorrectly fall back to another timestamp seek.

Mattermost thread pagination uses `(fromCreateAt, fromPost)` as a compound cursor. Supplying
`fromPost` without its `create_at` is invalid and must never be emitted by `PostRepository`.

Cursor responses are placed at the exact logical window implied by the cursor. They must not pass
through approximate index placement and must not relocate already authoritative overlap rows.

### In-flight attachment

Ordinary viewport demand can advance while an exact edge/cursor request is still in flight. Starting
an equivalent request is wasteful, but blindly queuing every later demand behind the first request can
make fast scrolling wait for stale work.

`ThreadPostSource` therefore uses `PostSourceRequestGate` only for demand that overlaps or directly
adjoins the logical range the current exact request is expected to cover. Attaching a waiter does not
extend that expected range:

```text
in-flight expected 145..154
new demand         153..160   -> attach
new demand         135..144   -> attach
new demand         120..134   -> independent request is allowed
```

When the physical request completes, attached logical requests are finished and `LongListWidget`
recomputes what the current viewport still needs. The source does not maintain a FIFO prefetch chain.
Seek requests are intentionally excluded from this attachment path.

## Random middle seek

Timestamp-based `fromCreateAt` loading is only a seed mechanism for a genuinely disconnected random
seek where neither adjacent logical boundary is known.

Both dragging the scrollbar thumb into an unavailable region and absolutely positioning the thumb by
clicking the scrollbar groove are random-seek gestures. Qt may report the latter as
`actionTriggered(SliderMove)` without first emitting `sliderMoved()`: with tracking enabled,
`setSliderPosition()` triggers the move action before the scrollbar marks its handle as pressed.
`LongListWidget` must therefore translate both input paths into the same logical seek target and seek
generation. A seek must never depend on an unrelated post edit, row reflow, model refresh or other
geometry notification to wake range loading.

After a timestamp seed overlaps or establishes an authoritative mapped row, further expansion toward
the viewport/buffer must continue with before/after post cursors. Repeating the same approximate page
against a known adjacent gap is a bug: it can leave a permanent estimated-height hole or, if known
identities are relocated to the estimate, create an identity ping-pong and visible tremor.

Timestamp placement must not be allowed to masquerade as edge authority. Opening a thread at the end
must go through the exact tail path; wheel scrolling away from that edge must continue from the exact
cursor established by the tail response.

## Count and boundary differences from channel history

Channel history needs an absolute-page boundary repair because `total_msg_count_root` is not the
row count of ordinary `/channels/{id}/posts`: deleted roots can make it too large, while count-excluded
system roots can make it too small. Threads deliberately do not inherit that page-probing algorithm.
Mattermost `Thread.ReplyCount` excludes deleted replies, and the thread endpoint
is cursor/time based (`fromCreateAt`, `fromPost`, `direction`) rather than an absolute `page=N` space.

If a future server/plugin configuration produces a real thread count mismatch, only the structural
slot mutation belongs in the shared `IndexedPostSource` layer. The proof that a particular count is
correct remains thread-specific, just as `/posts` page-boundary proof remains channel-specific.
`LongListWidget` must stay unaware of Mattermost counts, pages and cursors.

## Live replies in an open thread

An open `ThreadPostSource` holds a residency lease on its root post. That lease also acts as the
precise WebSocket admission signal for replies to that root: even if the parent channel is outside the
normal resident-channel horizon, a `posted` reply for the leased root must be inserted into
`BackendChannel` and delivered through `channel.onNewPost`.

This avoids the old failure mode where the server delivered the reply body over WebSocket, but the
client classified the parent channel as cold, emitted only the global transient notification, and the
open thread never saw the reply. The exception is deliberately root-specific; it must not make the
entire parent channel resident.

## Stability requirements

A page response that does not change logical identity mapping must not cause existing visible
`PostWidget`s to be destroyed and recreated.

Newly filled empty slots require availability notification. `itemsChanged` is reserved for logical
indices whose previously concrete identity/content really changed.

Changes to a root post's collapsed-thread metadata, such as reply count or participant presentation,
must not be published as a generic root-post edit. They may update the root's thread-summary widget
and the corresponding `ThreadPostSource` logical count, but they must not make the parent channel
rematerialize that root row.

All viewport position preservation, request look-ahead calculation, materialization and scrolling
remain inside `LongListWidget`. Thread-specific logical identity, edge/cursor choice and request-gate
attachment remain inside `ThreadPostSource`; Mattermost REST transport, pagination encoding, response
normalization and physical HTTP coalescing remain inside the single `PostRepository`.
