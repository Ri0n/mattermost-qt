from pathlib import Path

path = Path('sources/chat-area/ChannelPostSource.cpp')
text = path.read_text()
old = '''    if (hasRootCountEstimate) {\n        postIds.resize(currentLogicalCount());\n        seedCachedPosts();\n    } else {\n'''
new = '''    if (hasRootCountEstimate) {\n        // A zero message counter is still only an estimate: a channel may\n        // contain count-excluded system roots. Keep one unavailable bootstrap\n        // slot so LongList requests page zero and lets /posts prove empty vs.\n        // non-empty history. An actually empty channel immediately reconciles\n        // back to zero.\n        postIds.resize(std::max(1, currentLogicalCount()));\n        seedCachedPosts();\n    } else {\n'''
if text.count(old) != 1:
    raise RuntimeError('constructor bootstrap block not found exactly once')
path.write_text(text.replace(old, new))
