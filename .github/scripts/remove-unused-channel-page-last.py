from pathlib import Path

p = Path('sources/chat-area/ChannelPostSource.cpp')
text = p.read_text()
old = '''    const int last = first + count - 1;\n\n    bool touchesProvisionalIdentity = false;\n'''
new = '''    bool touchesProvisionalIdentity = false;\n'''
if text.count(old) != 1:
    raise SystemExit(f'expected one stale last variable, found {text.count(old)}')
p.write_text(text.replace(old, new, 1))
