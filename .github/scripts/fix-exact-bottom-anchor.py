from pathlib import Path

path = Path('sources/widgets/LongListWidget.cpp')
text = path.read_text()
old = '''    const qint64 offset = contentOffset();
    if (maximumContentOffset() - offset <= 2) {
        anchor.kind = ViewAnchor::Bottom;
        return anchor;
    }
'''
new = '''    const qint64 offset = contentOffset();
    if (verticalScrollBar()->value() == verticalScrollBar()->maximum()) {
        anchor.kind = ViewAnchor::Bottom;
        return anchor;
    }
'''
if text.count(old) != 1:
    raise SystemExit(f'captureAnchor bottom anchor count: {text.count(old)}')
path.write_text(text.replace(old, new, 1))

path = Path('docs/long-list-architecture.md')
text = path.read_text()
old = '''Only direct user input or an explicit logical navigation operation changes viewport intent.
'''
new = '''Only direct user input or an explicit logical navigation operation changes viewport intent. Sticky
bottom is exact user intent: only the actual scrollbar maximum captures a `Bottom` anchor. A thumb
position even one pixel above the maximum is an ordinary item anchor and must not be snapped back to
the end by a later materialization or geometry transaction.
'''
if text.count(old) != 1:
    raise SystemExit(f'ordinary anchor doc count: {text.count(old)}')
path.write_text(text.replace(old, new, 1))
