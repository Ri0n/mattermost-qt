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
 LongListWidget                    generic
        |
        v
   ChatLogWidget                   Mattermost post UI
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

`LongListWidget` owns geometry. `ChatLogWidget` owns post-specific presentation and actions. A post
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
- the current semantic viewport anchor;
- random-thumb-seek generation/debounce state;
- pending block requests;
- asynchronous item-geometry dirtiness.

It emits logical range requests and never knows what an item represents.

The initial API is intentionally small:

```cpp
setItemCount(count);
setDefaultItemHeight(height);
setRangeAvailable(first, last);
itemsChanged(first, last);
scrollToIndex(index, alignment);
scrollToEnd();

signals:
    rangeRequested(first, last, reason, generation);
    visibleRangeChanged(first, last);
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
capture logical anchor
        |
block scrollbar signals
freeze viewport updates
        |
create / destroy widgets
measure dirty sizeHints
update height index
recompute scrollbar range
        |
restore logical anchor against NEW geometry
position materialized widgets
        |
unfreeze viewport
unblock signals
paint once
```

There is no second anchor restore in a base class or controller and no queued paint-resume owner.

Child widgets are installed on the viewport and watched for `LayoutRequest` / `Resize`. Multiple
changes in one event-loop turn are coalesced. The next geometry transaction measures all dirty
widgets together before the viewport is allowed to move or repaint.

A delayed image, Markdown reflow, reaction row or thread button therefore cannot independently move
the chat viewport.

## Anchor semantics

For ordinary reading, the anchor is a logical item plus an offset inside that item. If the viewport
is explicitly attached to the newest edge, the anchor is `Bottom` instead.

Consequences:

- height changes above the viewport preserve the same logical reading position;
- height changes while sticky-bottom is active preserve the real end;
- a window resize cannot leave an unreachable last few pixels;
- pruning/materialization cannot reinterpret a pixel offset as a different logical item.

Only direct user input or an explicit `scrollToIndex()` / `scrollToEnd()` call changes semantic
intent.

## Wheel scrolling

Wheel scrolling is ordinary pixel movement. After the scrollbar value changes, `LongListWidget`
computes the visible logical range plus a configurable buffer. Missing data is requested in whole
blocks.

```text
wheel
  -> change scrollbar value
  -> compute logical visible + buffer range
  -> materialize available items
  -> request unavailable blocks
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

This replaces the old controller-level `TimelineSeekState` state machine.

## Request block policy

A request is rounded to a normal block size (initially 10 for interactive seek/prefetch). Asking for
one missing logical item therefore still produces an efficient block request.

`LongListWidget` only deduplicates logical items already requested but not yet reported available.
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
- selection/copy;
- context menus;
- edit/highlight operations;
- unread/day decorations;
- post-specific navigation requests.

It must **not** reimplement scrollbar math, anchoring, materialization, range buffering, seek or item
resize handling.

Day separators and the new-messages marker must not become fake logical list items. Prefer either:

1. decoration height associated with a real logical post, or
2. rendering inside the corresponding post widget.

This keeps one logical post index equal to one source item index.

## Post sources

The preferred boundary is a small source abstraction used by `ChatLogWidget`:

```cpp
class AbstractPostSource {
public:
    virtual int itemCount() const = 0;
    virtual bool isAvailable(int index) const = 0;
    virtual BackendPost* postAt(int index) const = 0;
    virtual void requestRange(int first, int last, RequestReason, quint64 generation) = 0;
};
```

`ChannelPostSource` and `ThreadPostSource` adapt different Mattermost endpoints into the same logical
contract. They do not manipulate widgets or scrollbars.

Normal open policy also belongs outside geometry:

```text
ordinary channel open -> ChatLogWidget::scrollToEnd()
ordinary thread open  -> ChatLogWidget::scrollToEnd()
permalink/Attention/Recent -> scrollToIndex(resolved logical target)
```

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
scrollToBottom()
scrollToItem(...)
setUpdatesEnabled(false/true) for list transactions
doItemsLayout() for chat-log geometry
calculate gap height / pixel seek position
create a placeholder item representing missing posts
```

A temporary migration adapter that needs one of these must live under `deprecated/` and must not be
linked into the final target.

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
| `PostsListWidget` sparse anchor restore | `LongListWidget` |
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
- arbitrary late reflow cannot leave the viewport with no materialized items;
- window resize preserves the end and immediately updates reachable scrollbar range;
- random seek uses normalized logical target and 100 ms debounce;
- stale seek availability cannot move a newer viewport;
- seed measurement expands only enough to cover viewport plus buffer.

Only after these tests are stable should `ChatLogWidget`, then channel source, then thread source be
connected.
