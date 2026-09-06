from pathlib import Path
import re


def replace_once(path, old, new):
    p = Path(path)
    text = p.read_text()
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected one match, found {count}")
    p.write_text(text.replace(old, new, 1))


# BackendChannel.h is old code with tab-aligned fields. Match the declaration
# semantically instead of depending on the exact number of tabs.
p = Path("sources/backend/types/BackendChannel.h")
text = p.read_text()
pattern = r"(?m)^(\s*uint64_t\s+last_post_at;\n)"
replacement = (
    r"\1"
    "    // Newest root-post timestamp. Unlike last_post_at, thread replies do not\n"
    "    // advance this value, so it is the correct freshness marker for the main\n"
    "    // channel root timeline/cache suffix. Older servers may omit the field;\n"
    "    // the constructor then falls back to last_post_at.\n"
    "    uint64_t                         last_root_post_at;\n"
)
text, count = re.subn(pattern, replacement, text, count=1)
if count != 1:
    raise RuntimeError(f"BackendChannel.h last_post_at declaration matches={count}")
p.write_text(text)

replace_once(
    "sources/backend/types/BackendChannel.cpp",
    '''\tlast_post_at = jsonObject.value("last_post_at").toVariant().toULongLong();\n\n\ttotal_msg_count = jsonObject.value("total_msg_count").toInt();\n''',
    '''\tlast_post_at = jsonObject.value("last_post_at").toVariant().toULongLong();\n\tlast_root_post_at = jsonObject.contains("last_root_post_at")\n\t\t? jsonObject.value("last_root_post_at").toVariant().toULongLong()\n\t\t: last_post_at;\n\n\ttotal_msg_count = jsonObject.value("total_msg_count").toInt();\n''')

# All root-timeline freshness checks must use last_root_post_at, not generic
# channel activity which also advances on replies.
p = Path("sources/chat-area/ChannelPostSource.cpp")
text = p.read_text()
for old, new in [
    ("channel.last_post_at != 0\n        && newest->create_at < channel.last_post_at",
     "channel.last_root_post_at != 0\n        && newest->create_at < channel.last_root_post_at"),
    ("hasRootCountEstimate || channel.last_post_at == 0",
     "hasRootCountEstimate || channel.last_root_post_at == 0"),
    ("newest->create_at < channel.last_post_at",
     "newest->create_at < channel.last_root_post_at"),
    ("guard->channel.last_post_at != 0\n                            && newest->create_at != guard->channel.last_post_at",
     "guard->channel.last_root_post_at != 0\n                            && newest->create_at != guard->channel.last_root_post_at"),
    ('<< " channel=" << guard->channel.last_post_at;',
     '<< " channel=" << guard->channel.last_root_post_at;'),
]:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"ChannelPostSource.cpp root freshness fragment count={count}: {old!r}")
    text = text.replace(old, new, 1)
p.write_text(text)

# Approximate thread pages are authoritative HTTP results too; route them
# through the exact placement wrapper so any matching cache-provisional IDs are
# upgraded and displaced provisional IDs are pruned.
replace_once(
    "sources/chat-area/ThreadPostSource.cpp",
    '''    publishExactWindow(assignExactWindow(first, ids.mid(0, count)));\n\n    qCDebug(lcThreadTimelineTrace).nospace()\n''',
    '''    placeExactWindow(first, ids.mid(0, count));\n\n    qCDebug(lcThreadTimelineTrace).nospace()\n''')

# Keep Qt5/Qt6 warning-clean around qsizetype/int comparisons introduced by
# provisional request planning.
p = Path("sources/chat-area/ThreadPostSource.cpp")
text = p.read_text()
replacements = [
    ("lastMissing + 1 < postIds.size()",
     "lastMissing + 1 < static_cast<int>(postIds.size())"),
    ("if (postIds.size() - 1 <= ServerBlockSize) {",
     "if (static_cast<int>(postIds.size()) - 1 <= ServerBlockSize) {"),
    ("if (index < 0 || index >= postIds.size()) {",
     "if (index < 0 || index >= static_cast<int>(postIds.size())) {"),
]
for old, new in replacements:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"ThreadPostSource.cpp warning fragment count={count}: {old!r}")
    text = text.replace(old, new, 1)
p.write_text(text)

# Document the distinction because it is part of cache authority, not merely an
# implementation detail.
replace_once(
    "docs/post-cache-runtime.md",
    '''A cached contiguous tail window may give an immediate first paint, but SQLite does not know the\ncurrent absolute `/posts?page=N&per_page=10` grid after remote traffic changed the channel. The source\nalso requires the cached newest post timestamp to match current channel `last_post_at`; otherwise the\nwindow is retained only as ordinary cached bodies and is not mapped as the current suffix.\n''',
    '''A cached contiguous tail window may give an immediate first paint, but SQLite does not know the\ncurrent absolute `/posts?page=N&per_page=10` grid after remote traffic changed the channel. The source\nalso requires the cached newest root timestamp to match current channel `last_root_post_at`; otherwise\nthe window is retained only as ordinary cached bodies and is not mapped as the current suffix. This\nmust not use `last_post_at`, because a thread reply advances general channel activity without changing\nthe root-post timeline. Older server payloads that omit `last_root_post_at` fall back to\n`last_post_at` in `BackendChannel`.\n''')

print("root-tail freshness and thread provisional upgrade fixes applied")
