from pathlib import Path

path = Path('sources/widgets/LongListWidget.h')
text = path.read_text()
old = '''    bool isAtEnd() const
    {
        return maximumContentOffset() - contentOffset() <= 2;
    }
'''
new = '''    bool isAtEnd() const
    {
        return maximumContentOffset() == contentOffset();
    }
'''
if text.count(old) != 1:
    raise SystemExit(f'isAtEnd anchor count: {text.count(old)}')
path.write_text(text.replace(old, new, 1))
