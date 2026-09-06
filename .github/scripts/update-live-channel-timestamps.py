from pathlib import Path


def replace_once(path, old, new):
    p = Path(path)
    text = p.read_text()
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected one match, found {count}")
    p.write_text(text.replace(old, new, 1))


replace_once(
    "sources/backend/WebSocketEventHandler.cpp",
    '''\tBackendChannel* channel = storage.getChannelById (event.channelId);\n\tif (!channel) {\n\t\treturn;\n\t}\n\n\tif (!repository.shouldRetainChannelInMemory(event.channelId)) {\n''',
    '''\tBackendChannel* channel = storage.getChannelById (event.channelId);\n\tif (!channel) {\n\t\treturn;\n\t}\n\n\t// Timeline edge metadata must advance even when the channel is outside the\n\t// resident-body admission horizon. last_post_at includes replies, while the\n\t// root-only chat timeline uses last_root_post_at as its freshness marker.\n\tconst uint64_t createAt = event.postObject.value(QStringLiteral("create_at"))\n\t\t.toVariant().toULongLong();\n\tchannel->last_post_at = std::max(channel->last_post_at, createAt);\n\tif (event.postObject.value(QStringLiteral("root_id")).toString().isEmpty()) {\n\t\tchannel->last_root_post_at = std::max(channel->last_root_post_at, createAt);\n\t}\n\n\tif (!repository.shouldRetainChannelInMemory(event.channelId)) {\n''')

# std::max is now used directly in this translation unit.
replace_once(
    "sources/backend/WebSocketEventHandler.cpp",
    '#include "WebSocketEventHandler.h"\n\n#include <QJsonDocument>\n',
    '#include "WebSocketEventHandler.h"\n\n#include <algorithm>\n\n#include <QJsonDocument>\n')

replace_once(
    "docs/post-cache-runtime.md",
    '''Older server payloads that omit `last_root_post_at` fall back to\n`last_post_at` in `BackendChannel`.\n''',
    '''Older server payloads that omit `last_root_post_at` fall back to\n`last_post_at` in `BackendChannel`. Live `posted` events advance both resident channel markers before\npost-body memory admission is evaluated, so even a cold channel cannot later accept a suffix older\nthan an already observed WebSocket root post.\n''')

print("live channel timeline timestamps updated")
