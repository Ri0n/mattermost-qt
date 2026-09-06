from pathlib import Path

p = Path('sources/chat-area/ThreadPostSource.cpp')
text = p.read_text()
old = '''    int count = currentLogicalCount();
    if (count <= postIds.size()) {
        count = static_cast<int>(postIds.size()) + 1;
    }
    postIds.resize(count);
    if (!postIds.isEmpty()) {
        postIds[0] = rootId;
    }
    const int index = count - 1;
    postIds[index] = post.id;
'''
new = '''    const int oldCount = static_cast<int>(postIds.size());
    int count = currentLogicalCount();

    // BackendChannel::addPost() updates the root's reply_count and emits
    // onPostEdited(root) before the posted event is forwarded as onNewPost(reply).
    // The edit handler above therefore normally grows postIds first, leaving an
    // empty newest slot reserved for this exact live reply. Do not count the
    // same reply twice by blindly appending another logical row.
    const bool metadataReservedTail = count == oldCount && count > 1
        && postIds.at(count - 1).isEmpty();
    if (count > oldCount) {
        postIds.resize(count);
    } else if (!metadataReservedTail) {
        // Defensive fallback for a producer that delivers the live reply before
        // root metadata has advanced. In that ordering the event itself is the
        // only evidence that the logical thread grew.
        count = oldCount + 1;
        postIds.resize(count);
    }
    if (!postIds.isEmpty()) {
        postIds[0] = rootId;
    }
    const int index = count - 1;
    postIds[index] = post.id;
'''
if text.count(old) != 1:
    raise SystemExit(f'append block matches={text.count(old)}')
text = text.replace(old, new, 1)
p.write_text(text)
