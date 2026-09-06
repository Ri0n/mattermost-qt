# LongListWidget architecture

This document is the implementation contract for long, partially materialized widget lists in
Mattermost-Qt. The first consumer is the chat log, but `LongListWidget` itself is deliberately not
message-aware.

The previous sparse timeline implementation is retained under `deprecated/` only as migration
reference. New code must not include or link it.

## Core rule

**Exactly one object owns scroll geometry and the viewport: `LongListWidget`.**

No channel/thread controller, data source, child item widget, cache or navigation service may set a
scrollbar value, calculate pixel positions in the list, create placeholder rows, preserve a viewport
anchor, freeze painting or decide which concrete widgets should exist.

If another class needs something visible, it asks `LongListWidget` in logical-item coordinates.

## Class layering

```text
QAbstractScrollArea
        |
        v
 LongListWidget                    generic geometry + viewport intent
        |
        v
   ChatLogWidget                   Mattermost post UI + semantic post identity
        |
        +-------------------+
        |                   |
 ChannelPostSource      ThreadPostSource
        |                   |
        +---------+---------+
                  |
          PostTimelineService
                  |
          memory / HTTP / SQLite
```

`LongListWidget` owns geometry, scrolling, viewport anchoring and persistent logical-item viewport
locks. `ChatLogWidget` owns post-specific presentation, actions and semantic post-ID identity. A post
source owns logical-index-to-post identity and range availability. `PostTimelineService` owns range
retrieval, in-flight request coalescing and cache tiers.

Channel and thread logs share the same widget and therefore the same scrollbar, materialization,
seek, resize and pruning semantics. Their only meaningful difference is how a logical range is
resolved/fetched.

## LongListWidget responsibilities

`LongListWidget : QAbstractScrollArea` maintains:

- a logical item count;
- a non-zero default estimated item height;
- an effective height for every logical item;
- availability bits for logical items;
- a bounded set of materialized child `QWidget`s;
- one vertical scrollbar;
- the current ordinary logical viewport anchor;
- an optional persistent logical-item viewport lock;
- random-thumb-seek generation/debounce state;
- pending block requests;
- asynchronous item-geometry dirtiness;
- recognition of direct user scroll intent.

It emits logical range requests and never knows what an item represents.

The core API is intentionally small:

```cpp
setItemCount(count);
insertItems(first, count);
setDefaultItemHeight(height);
setRangeAvailable(first, last);
itemsChanged(first, last);
finishRangeRequest(first, last);
scrollToIndex(index, alignment);
scrollToEnd();
lockViewportToItem(index, alignment, quietPeriodMs);
remapViewportLockedItem(index);
clearViewportLock();

signals:
    rangeRequested(first, last, reason, generation);
    visibleRangeChanged(first, last);
    materializedRangeChanged(first, last);
    userViewportChanged(atEnd);
    viewportLockReleased();
```

A subclass supplies a concrete widget for an available logical item through
`createItemWidget(index)`.

## No gap widgets

There are **no placeholder/gap rows**.

For an unavailable or unmaterialized item the list only knows an estimated height. A Fenwick tree
(prefix-sum index) stores effective heights and provides:

```text
item index -> content pixel       O(log N)
content pixel -> item index       O(log N)
height change -> total geometry   O(log N)
```

For example, with 10,000 logical items only ~50 concrete widgets may exist while the scroll range
still represents all 10,000 items.

This removes the old failure mode where giant `QListWidgetItem` gap placeholders participated in
Qt layout and then changed size when real `PostWidget`s were inserted.

## Geometry ownership and transaction

Any operation that can change effective item heights uses one synchronous transaction owned by
`LongListWidget`:

```text
capture ordinary anchor / inspect persistent viewport lock
        |
block scrollbar signals
freeze viewport updates
        |
create / destroy widgets
measure dirty sizeHints
update height index
recompute scrollbar range
        |
restore seek target, persistent lock, or ordinary anchor against NEW geometry
position materialized widgets
        |
unfreeze viewport
unblock signals
paint once
```

There is no second pixel-anchor restore in a base class or controller and no queued paint-resume
owner.

Child widgets are installed on the viewport and watched for `LayoutRequest` / `Resize`. Multiple
changes in one event-loop turn are coalesced. The next geometry transaction measures all dirty
widgets together before the viewport is allowed to move or repaint.

A delayed image, Markdown reflow, reaction row or thread button therefore cannot independently move
the chat viewport.

## Ordinary anchor semantics

For ordinary reading, the anchor is the logical item intersecting the viewport top plus the offset
inside that item. If the viewport is explicitly attached to the newest edge, the anchor is `Bottom`
instead.

Consequences:

- height changes entirely above the viewport preserve the visible content at exactly the same screen
  coordinates;
- height changes while sticky-bottom is active preserve the real end;
- appending a logical item while sticky-bottom is active keeps the viewport on the new end;
- prepending real logical items shifts logical indices without moving the already visible content;
- a window resize cannot leave an unreachable last few pixels;
- pruning/materialization cannot reinterpret a pixel offset as a different logical item.

A geometry change *inside* the visible region cannot keep every row on both sides at the same Y: if a
visible item grows by 200 pixels, some content must make room for those pixels. The invariant is that
`LongListWidget` preserves its chosen semantic anchor and introduces no additional artificial jump.

Only direct user input or an explicit logical navigation operation changes viewport intent.

## Persistent viewport lock semantics

Explicit semantic navigation needs a stronger invariant than the ordinary top-of-viewport anchor.
For example, a permalink target may be placed near the middle of the viewport and then experience
attachment loading, Markdown reflow, logical prepends or a provisional-to-authoritative index remap.

`LongListWidget::lockViewportToItem()` applies the requested `Alignment` **once** and then records the
target item's **top edge as a fraction of the viewport height**:

```text
itemTopFraction = itemTopViewportY / viewportHeight
```

After that, alignment is no longer re-applied. Geometry transactions restore the item top from the
stored fraction:

```text
same viewport height:
    500 px / 1000 px -> 500 px / 1000 px

viewport resized to half height:
    500 px / 1000 px -> 250 px / 500 px
```

This gives two desired properties at once:

- ordinary reflow/loading with unchanged viewport height keeps the locked item at the same absolute
  screen Y;
- resizing the viewport keeps the locked item at the same relative vertical position.

The lock is expressed only in logical-item coordinates at the public boundary. All conversion to
content pixels and scrollbar values remains private to `LongListWidget`.

`remapViewportLockedItem(newIndex)` changes only the logical index represented by the lock. The stored
screen position is preserved; the item is **not** centered again. Logical insertions before the lock
automatically shift its index as part of the same structural transaction.

A direct wheel, scrollbar action or thumb drag immediately gives authority back to the user and
releases the persistent lock. A positive quiet period may also release it; zero means that only user
intent or explicit teardown releases it.

## Wheel and scrollbar scrolling

Wheel scrolling is ordinary pixel movement. Recognition of wheel, scrollbar action and thumb-drag
user intent belongs entirely to `LongListWidget`; domain subclasses do not override wheel handling or
inspect scrollbar signals.

After the scrollbar value changes, `LongListWidget` computes the visible logical range plus a
configurable buffer. Missing data is requested in whole blocks.

```text
user scroll gesture
  -> release persistent viewport lock
  -> change scrollbar value
  -> compute logical visible + buffer range
  -> materialize available items
  -> request unavailable blocks
  -> emit userViewportChanged(atEnd)
```

Loading adjacent data never recenters the viewport.

## Random thumb seek

Thumb drag uses normalized logical position rather than current estimated pixel geometry:

```text
fraction = (value - minimum) / (maximum - minimum)
target   = round(fraction * (itemCount - 1))
```

While the thumb is moving, target changes restart a 100 ms debounce timer. When it becomes stable:

```text
request ~10-item seed around TARGET
        |
materialize + measure seed
        |
center TARGET using the new geometry
        |
calculate actual viewport + buffer coverage
        |
request additional whole blocks only where coverage is missing
```

A new thumb target increments the seek generation. Results from an older generation may still enter
the memory/disk cache, but `LongListWidget` only materializes what the current viewport/seek needs,
so stale results have no authority to move the viewport.

Channel history range loading uses a single paging contract: Mattermost absolute pages with
`per_page=10`. Already known post identities are not reused as `before`/`after` paging boundaries.
They are inputs to semantic-position estimation and overlap reconciliation only. Because logical
blocks are aligned from the oldest end while Mattermost pages are aligned from the newest end, one
ten-item logical request may intersect two server pages; the source loads both and places each via
its absolute page number. Once the oldest boundary is known, a jump to any scrollbar position is
therefore O(1) page requests rather than an identity-cursor walk through history.

`total_msg_count_root` is only an initial coordinate estimate for `/posts`, not its row count.
Deleted roots can make the counter larger than visible history, while join/leave and other system
roots excluded from Mattermost message counts are still returned by `/posts` and can make the counter
smaller. The source therefore repairs the oldest boundary in both directions. Absolute pages remain
newest-anchored; count growth inserts empty logical slots at the oldest side so already mapped pages do
not move relative to the newest edge.

Boundary search stays in ten-post page coordinates but tests distant candidate page starts with one
root only: candidate page P is probed as `page=P*10&per_page=1`. For a large top-edge request the first
probe jumps inward by a heuristic 3% of the estimated root count. If it is empty, the step grows
exponentially farther inward until data is found. If it exists, binary search walks outward to the
reported boundary. A full reported-boundary page is not accepted as proof: the source first probes the
adjacent older page and, if that exists too, expands outward exponentially until `/posts` provides an
empty/short boundary. Small estimates use the normal ten-post path first and enter the same outward
repair only when their reported oldest page is full.

Near a bounded edge, when at most two unknown ten-post pages remain, the source stops spending
one-root probes and materializes the first unknown page with `per_page=10`. A short page proves the
exact count; a full page adjacent to known emptiness proves an exact multiple of ten. Exact
reconciliation may therefore remove a phantom prefix or insert a missing oldest prefix. The 3% value
changes latency only, never correctness, and no identity cursor is introduced by this repair path.

This replaces the old controller-level `TimelineSeekState` state machine.

## Request block policy

A request is rounded to a normal block size (initially 10 for interactive seek/prefetch). Asking for
one missing logical item therefore still produces an efficient block request.

`LongListWidget` only deduplicates logical items already requested but not yet reported available.
Every `rangeRequested(first,last,...)` must eventually be paired with
`finishRangeRequest(first,last)`, including failures and differently aligned server responses. A
failure releases suppression but does not immediately reschedule itself, avoiding a tight retry
loop.

Network-level coalescing remains in `PostTimelineService`; the widget must not know whether a result
came from an already materialized model object, RAM cache, SQLite or HTTP.

## Materialization and eviction

`LongListWidget` keeps only the desired viewport window and buffer, bounded by a hard widget budget
(initially 200).

Evicting a widget means only:

```text
remove child widget from materialized map
retain its measured height
retain source/cache data
```

There is no gap merge and no timeline rebuild. Learned height belongs to the logical item geometry,
not to the lifetime of its current QWidget.

## ChatLogWidget

`ChatLogWidget : LongListWidget` is the first domain-specific layer. It may know about:

- `PostWidget` construction;
- post identity;
- semantic post-ID navigation identity;
- selection/copy;
- context menus;
- edit/highlight operations;
- unread/day decorations;
- post-specific range-loading requests.

It must **not** reimplement scrollbar math, pixel anchoring, viewport-lock timing, user scroll gesture
recognition, materialization, range buffering, seek or item resize handling.

Day separators and the new-messages marker must not become fake logical list items. Prefer either:

1. decoration height associated with a real logical post, or
2. rendering inside the corresponding post widget.

This keeps one logical post index equal to one source item index.

## Semantic navigation and provisional indices

A permalink, Attention item or unread-thread target is identified by **post ID**, not by its current
logical index. This matters because a cached context may know the target post before the source knows
its authoritative server page boundary.

`AbstractPostSource::ensurePostIndex(postId)` may therefore adopt an already cached target into an
estimated empty logical slot when the source has an exact logical coordinate space. This slot is
provisional. When an authoritative page arrives, the source is allowed to remove that provisional
occurrence and map the same post ID to its real index.

The ownership split is:

```text
post ID / provisional -> authoritative index    PostSource
semantic post ID identity                       ChatLogWidget
logical item -> persistent viewport position    LongListWidget
index -> pixels / scrollbar / geometry           LongListWidget
```

While semantic navigation is active, `ChatLogWidget` remembers only the target post ID and the last
logical index representing it. If source signals move that ID to another index, `ChatLogWidget` calls
`remapViewportLockedItem(newIndex)`. It does **not** call `scrollToIndex(Center)` again and never
calculates or stores a pixel position.

The existing viewport position therefore survives both source identity remaps and all subsequent
geometry changes. `LongListWidget::viewportLockReleased()` tells `ChatLogWidget` when user intent,
timeout or teardown has ended that semantic navigation state.

Availability follows the same identity rule. `itemsChanged(first,last)` can mean that a provisional
slot became empty even if no QWidget was ever materialized there, so `ChatLogWidget` synchronizes
`LongListWidget` availability bits with `PostSource::isAvailable()` for the whole changed range, not
only for concrete widgets.

## Post sources

The preferred boundary is a small source abstraction used by `ChatLogWidget`:

```cpp
class AbstractPostSource {
public:
    virtual int itemCount() const = 0;
    virtual bool isAvailable(int index) const = 0;
    virtual BackendPost* postAt(int index) const = 0;
    virtual int indexOfPost(const QString& postId) const = 0;
    virtual int ensurePostIndex(const QString& postId);
    virtual void requestRange(int first, int last, RequestReason, quint64 generation) = 0;
    virtual bool canRequestBeforeFirst() const;
    virtual void requestBeforeFirst(RequestReason, quint64 generation);

signals:
    itemCountChanged(count);
    itemsInserted(first, count);
    rangeAvailable(first, last);
    itemsChanged(first, last);
    rangeRequestFinished(first, last);
};
```

`ChannelPostSource` and `ThreadPostSource` adapt different Mattermost endpoints into the same logical
contract. They do not manipulate widgets or scrollbars.

`BackendPost::hidden` is a channel-root-list concern: replies are intentionally marked hidden by
`BackendChannel` so they do not appear as root rows. `ThreadPostSource` must still expose posts whose
`root_id` matches its thread. It must not interpret `hidden` as "not visible inside this thread".

Normal open policy also belongs outside geometry:

```text
ordinary channel open -> ChatLogWidget::scrollToEnd()
ordinary thread open  -> ChatLogWidget::scrollToEnd()
permalink/Attention/Recent -> semantic post-ID identity + LongListWidget viewport lock
```

### Exact versus unknown logical count

`LongListWidget::itemCount()` describes actual logical items, not spare capacity and not a UI gap.
When `total_msg_count_root` is available, `ChannelPostSource` can provide exact oldest-to-newest
indices immediately and may use provisional empty slots for cached semantic targets.

Some Mattermost versions do not provide `total_msg_count_root`. A cached newest suffix is then **not**
an exact complete channel and must not silently become one, otherwise index 0 falsely looks like the
oldest edge and older history can never be requested.

Unknown-count mode therefore uses a contiguous sequence of **actually discovered root posts**. When
the visible window reaches its oldest edge, `ChannelPostSource` requests a cursor page before the
first known post. New real rows are prepended through:

```text
PostSource::itemsInserted(0, count)
        -> LongListWidget::insertItems(0, count)
        -> shift logical indices / Fenwick heights / materialized map
        -> restore Bottom, persistent lock, or ordinary anchor
```

No spare logical capacity and no fake placeholder rows are introduced. Outstanding view-side pending
range bits are discarded on a structural index shift; the service layer still coalesces equivalent
HTTP work. If the server reports no older cursor/data, the source marks the oldest boundary reached
and stops repeating that request.

Do not substitute `total_msg_count` for the missing root count: it may include thread replies and
would create phantom logical rows.

## Cache boundary

`PostTimelineService` remains below sources and is responsible for:

1. already present BackendChannel data;
2. equivalent in-flight request coalescing;
3. future SQLite post cache;
4. HTTP for the remaining missing range.

A successful stale request is still useful cache population. Cache success and viewport authority
are deliberately separate concepts.

## Prohibited ownership outside LongListWidget

New production code outside `LongListWidget` must not call, for a chat log:

```text
verticalScrollBar()->setValue(...)
connect to scrollbar signals to infer chat viewport intent
override wheel handling to implement chat scrolling
scrollToBottom()
scrollToItem(...)
setUpdatesEnabled(false/true) for list transactions
doItemsLayout() for chat-log geometry
calculate gap height / pixel seek position
store a pixel viewport anchor or navigation offset
create a placeholder item representing missing posts
```

`ChatLogWidget` may request logical operations from its base (`scrollToIndex`, `scrollToEnd`,
`lockViewportToItem`, `remapViewportLockedItem`) but must not bypass those APIs to manipulate pixels.

A temporary migration adapter that needs one of the prohibited operations must live under
`deprecated/` and must not be linked into the final target.

## Migration from deprecated sparse timeline

The old implementation is moved under `deprecated/` and excluded from normal source globbing. This
is intentional: compile errors are the migration checklist.

Old responsibility -> new owner:

| Deprecated responsibility | New owner |
| --- | --- |
| `PostTimeline` gap spans / pixel mapping / measured heights | `LongListWidget` height index |
| `TimelineSeekState` | `LongListWidget` internal seek state |
| channel/thread viewport checks | `LongListWidget` |
| channel/thread pruning | `LongListWidget` materialization budget |
| `PostsListWidget` sparse pixel-anchor restore | `LongListWidget` |
| `PostsListWidget` semantic post navigation identity | `ChatLogWidget` post-ID identity |
| `PostsListWidget` semantic viewport position lock | `LongListWidget` persistent viewport lock |
| `ResizableListWidget` chat-row size anchoring | `LongListWidget` child event filter |
| channel/thread HTTP adapters | post sources / `PostTimelineService` |
| PostWidget-specific actions | `ChatLogWidget` |

The migration should remove dependencies rather than add compatibility wrappers back to deprecated
headers.

## Required tests before Mattermost integration

`LongListWidget` is tested first with a synthetic source, without Mattermost objects:

- 10,000 uniform items: middle scrollbar position maps near item 5,000;
- missing visible items request whole blocks;
- no gap/placeholder widgets exist;
- materialized QWidget count never exceeds the configured budget;
- delayed sizeHint growth above the viewport preserves the logical anchor;
- delayed sizeHint growth at bottom preserves the true newest edge;
- logical item-count growth while sticky-bottom is active follows the new end;
- prepend shifts logical indices without moving the existing viewport;
- prepend preserves sticky-bottom on the same newest logical item;
- a persistent viewport lock keeps the same absolute item Y through reflow;
- a persistent viewport lock keeps the same relative Y through viewport resize;
- remapping a persistent viewport lock to a new logical index preserves screen position;
- arbitrary late reflow cannot leave the viewport with no materialized items;
- window resize preserves the end and immediately updates reachable scrollbar range;
- random seek uses normalized logical target and 100 ms debounce;
- stale seek availability cannot move a newer viewport;
- seed measurement expands only enough to cover viewport plus buffer.

Domain integration additionally needs tests that:

- a provisional permalink target can move to an authoritative index without moving the semantic
  viewport away from that post;
- clearing a provisional source slot clears list availability even if no widget existed there;
- cached and live thread replies remain visible despite the channel-root `hidden` flag;
- a server without `total_msg_count_root` can discover older root posts without fake UI rows.
