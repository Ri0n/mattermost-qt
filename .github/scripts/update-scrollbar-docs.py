from pathlib import Path

path = Path('docs/long-list-architecture.md')
text = path.read_text()

old = '''## Random thumb seek

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
'''
new = '''## Random thumb seek

A scrollbar value represents the **top content offset of a viewport**, not a post ordinal. Thumb
movement therefore first maps the scrollbar value back into the current estimated content geometry
and chooses the logical item at the viewport centre:

```text
top    = contentOffsetForScrollValue(value)
center = top + viewportHeight / 2
target = heightIndex.indexAtPixel(center)
```

This distinction matters near the newest edge: moving the thumb upward by one pixel must not still
select the final post and then re-centre it, which would snap the scrollbar back to the end.

Thumb dragging has two modes:

```text
target already materialized OR desired viewport bodies already available
        -> ordinary buffered scroll path immediately

jump enters unavailable / unmaterialized history
        -> seek target + 100 ms debounce
```

The first mode intentionally behaves like wheel scrolling: it keeps the user's exact scrollbar
position and materializes the viewport/buffer immediately. No seek re-centering is allowed merely
because the user happened to drag the thumb instead of using the wheel.

Only a genuine random jump into data that is not ready enters seek mode. While such a thumb target is
moving, target changes restart the 100 ms debounce timer. When it becomes stable:

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

A new seek target increments the seek generation. Results from an older generation may still enter
the memory/disk cache, but `LongListWidget` only materializes what the current viewport/seek needs,
so stale results have no authority to move the viewport.
'''
if text.count(old) != 1:
    raise SystemExit(f'random thumb section anchor count: {text.count(old)}')
text = text.replace(old, new, 1)

old = '''## Materialization and eviction

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
'''
new = '''## Materialization and eviction

The desired viewport window plus buffer defines what must be materialized **now** and which missing
blocks should be requested. It is not the destruction boundary for already-created widgets.

`maxMaterializedItems` is a resident widget budget (initially 200). Already-created widgets are kept
while the total remains within that budget. Consequently a short chat with, for example, 21 posts can
remain fully materialized after the user has visited all of it instead of continually destroying and
recreating rows just outside the current buffer.

When materialization would exceed the budget, `LongListWidget` first protects the current desired
viewport/buffer and evicts the farthest widgets outside that window until the count is back at the
budget. The retained set is therefore allowed to be larger than the current viewport window and does
not have to be one contiguous range.

Evicting a widget means only:

```text
remove child widget from materialized map
retain its measured height
retain source/cache data
```

There is no gap merge and no timeline rebuild. Learned height belongs to the logical item geometry,
not to the lifetime of its current QWidget.
'''
if text.count(old) != 1:
    raise SystemExit(f'materialization section anchor count: {text.count(old)}')
text = text.replace(old, new, 1)

path.write_text(text)
