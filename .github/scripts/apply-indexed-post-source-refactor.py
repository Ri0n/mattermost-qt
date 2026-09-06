from pathlib import Path


def replace_once(path, old, new):
    p = Path(path)
    text = p.read_text()
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected one match, found {count}")
    p.write_text(text.replace(old, new, 1))


# ---------------------------------------------------------------------------
# Headers: both concrete sources derive from the shared indexed source base.
# ---------------------------------------------------------------------------
replace_once(
    "sources/chat-area/ChannelPostSource.h",
    '#include "AbstractPostSource.h"\n',
    '#include "IndexedPostSource.h"\n',
)
replace_once(
    "sources/chat-area/ChannelPostSource.h",
    'class ChannelPostSource : public AbstractPostSource\n',
    'class ChannelPostSource : public IndexedPostSource\n',
)
replace_once(
    "sources/chat-area/ChannelPostSource.h",
    '''    int itemCount() const override { return static_cast<int>(postIds.size()); }\n    bool isAvailable(int index) const override;\n    BackendPost* postAt(int index) const override;\n    int indexOfPost(const QString& postId) const override;\n    int ensurePostIndex(const QString& postId) override;\n''',
    '''    int ensurePostIndex(const QString& postId) override;\n''',
)
replace_once(
    "sources/chat-area/ChannelPostSource.h",
    '    void seedUnknownNewestPost();\n    void rebuildIndex();\n    void removeLogicalRange(int first, int count);\n',
    '    void seedUnknownNewestPost();\n    void removeLogicalRange(int first, int count);\n',
)
replace_once(
    "sources/chat-area/ChannelPostSource.h",
    '''    Backend& backend;\n    BackendChannel& channel;\n    QVector<QString> postIds;\n    QHash<QString, int> postIndexes;\n\n''',
    '''    Backend& backend;\n\n''',
)

replace_once(
    "sources/chat-area/ThreadPostSource.h",
    '#include "AbstractPostSource.h"\n',
    '#include "IndexedPostSource.h"\n',
)
replace_once(
    "sources/chat-area/ThreadPostSource.h",
    'class ThreadPostSource : public AbstractPostSource\n',
    'class ThreadPostSource : public IndexedPostSource\n',
)
replace_once(
    "sources/chat-area/ThreadPostSource.h",
    '''    int itemCount() const override { return static_cast<int>(postIds.size()); }\n    bool isAvailable(int index) const override;\n    BackendPost* postAt(int index) const override;\n    int indexOfPost(const QString& postId) const override;\n    int ensurePostIndex(const QString& postId) override;\n''',
    '''    int ensurePostIndex(const QString& postId) override;\n''',
)
replace_once(
    "sources/chat-area/ThreadPostSource.h",
    '''    void seedCachedPosts();\n    void rebuildIndex();\n    void placeExactWindow(int first, const QStringList& ids);\n''',
    '''    void seedCachedPosts();\n    void placeExactWindow(int first, const QStringList& ids);\n''',
)
replace_once(
    "sources/chat-area/ThreadPostSource.h",
    '''    Backend& backend;\n    BackendChannel& channel;\n    QString rootId;\n    QVector<QString> postIds;\n    QHash<QString, int> postIndexes;\n''',
    '''    Backend& backend;\n    QString rootId;\n''',
)

# ---------------------------------------------------------------------------
# Constructors and duplicated lookup/index implementations.
# ---------------------------------------------------------------------------
replace_once(
    "sources/chat-area/ChannelPostSource.cpp",
    '''    : AbstractPostSource(parent)\n    , backend(backendInstance)\n    , channel(channelInstance)\n    , hasRootCountEstimate(channelInstance.has_total_msg_count_root)\n''',
    '''    : IndexedPostSource(channelInstance, parent)\n    , backend(backendInstance)\n    , hasRootCountEstimate(channelInstance.has_total_msg_count_root)\n''',
)
replace_once(
    "sources/chat-area/ThreadPostSource.cpp",
    '''    : AbstractPostSource(parent)\n    , backend(backendInstance)\n    , channel(channelInstance)\n    , rootId(std::move(sourceRootId))\n''',
    '''    : IndexedPostSource(channelInstance, parent)\n    , backend(backendInstance)\n    , rootId(std::move(sourceRootId))\n''',
)

replace_once(
    "sources/chat-area/ChannelPostSource.cpp",
    '''bool ChannelPostSource::isAvailable(int index) const\n{\n    return index >= 0 && index < postIds.size() && !postIds.at(index).isEmpty()\n        && channel.postIdToPost.contains(postIds.at(index));\n}\n\nBackendPost* ChannelPostSource::postAt(int index) const\n{\n    if (!isAvailable(index)) {\n        return nullptr;\n    }\n    return channel.postIdToPost.value(postIds.at(index), nullptr);\n}\n\nint ChannelPostSource::indexOfPost(const QString& postId) const\n{\n    return postIndexes.value(postId, -1);\n}\n\n''',
    '',
)
replace_once(
    "sources/chat-area/ThreadPostSource.cpp",
    '''bool ThreadPostSource::isAvailable(int index) const\n{\n    return index >= 0 && index < postIds.size() && !postIds.at(index).isEmpty()\n        && channel.postIdToPost.contains(postIds.at(index));\n}\n\nBackendPost* ThreadPostSource::postAt(int index) const\n{\n    if (!isAvailable(index)) {\n        return nullptr;\n    }\n    return channel.postIdToPost.value(postIds.at(index), nullptr);\n}\n\nint ThreadPostSource::indexOfPost(const QString& postId) const\n{\n    return postIndexes.value(postId, -1);\n}\n\n''',
    '',
)

# ---------------------------------------------------------------------------
# Shared tail resize replaces local resize + index rebuilding.
# ---------------------------------------------------------------------------
replace_once(
    "sources/chat-area/ChannelPostSource.cpp",
    '''        const int count = currentLogicalCount();\n        if (count != static_cast<int>(postIds.size())) {\n            postIds.resize(count);\n            rebuildIndex();\n            emit itemCountChanged(count);\n        }\n''',
    '''        resizeLogicalTail(currentLogicalCount());\n''',
)
replace_once(
    "sources/chat-area/ThreadPostSource.cpp",
    '''            const int count = currentLogicalCount();\n            if (count != static_cast<int>(postIds.size())) {\n                qCDebug(lcThreadTimelineTrace).nospace()\n                    << "THREAD_COUNT_CHANGE source=" << static_cast<const void*>(this)\n                    << " old=" << postIds.size()\n                    << " new=" << count\n                    << " replyCount=" << post.reply_count;\n                postIds.resize(count);\n                if (!postIds.isEmpty()) {\n                    postIds[0] = rootId;\n                }\n                rebuildIndex();\n                qCDebug(lcThreadTimelineTrace).nospace()\n                    << "THREAD_SLOTS source=" << static_cast<const void*>(this)\n                    << ' ' << slotSummary(postIds);\n                emit itemCountChanged(count);\n            }\n''',
    '''            const int count = currentLogicalCount();\n            if (count != static_cast<int>(postIds.size())) {\n                qCDebug(lcThreadTimelineTrace).nospace()\n                    << "THREAD_COUNT_CHANGE source=" << static_cast<const void*>(this)\n                    << " old=" << postIds.size()\n                    << " new=" << count\n                    << " replyCount=" << post.reply_count;\n                resizeLogicalTail(count);\n                qCDebug(lcThreadTimelineTrace).nospace()\n                    << "THREAD_SLOTS source=" << static_cast<const void*>(this)\n                    << ' ' << slotSummary(postIds);\n            }\n''',
)

# ---------------------------------------------------------------------------
# Shared exact-window identity relocation and signal publication.
# ---------------------------------------------------------------------------
replace_once(
    "sources/chat-area/ThreadPostSource.cpp",
    '''void ThreadPostSource::placeExactWindow(int first, const QStringList& ids)\n{\n    if (postIds.isEmpty() || ids.isEmpty()) {\n        return;\n    }\n\n    first = std::max(0, std::min(first, static_cast<int>(postIds.size()) - 1));\n    const int count = std::min(static_cast<int>(ids.size()),\n                               static_cast<int>(postIds.size()) - first);\n    if (count <= 0) {\n        return;\n    }\n    const int last = first + count - 1;\n    const QVector<QString> before = postIds;\n\n    // Apply identity changes atomically. Exact cursor windows have authoritative\n    // logical placement, so old provisional occurrences move to this window;\n    // they are not allowed to override the cursor origin.\n    for (int offset = 0; offset < count; ++offset) {\n        const QString& id = ids.at(offset);\n        const int target = first + offset;\n        const int existing = postIndexes.value(id, -1);\n        if (existing >= 0 && existing != target) {\n            postIds[existing].clear();\n        }\n    }\n    for (int offset = 0; offset < count; ++offset) {\n        postIds[first + offset] = ids.at(offset);\n    }\n    rebuildIndex();\n\n    // Newly filled empty slots need only availability. Existing concrete rows\n    // are rematerialized only when their identity really changed.\n    for (int index = 0; index < postIds.size(); ++index) {\n        if (before.at(index) != postIds.at(index) && !before.at(index).isEmpty()) {\n            emit itemsChanged(index, index);\n        }\n    }\n    emit rangeAvailable(first, last);\n}\n''',
    '''void ThreadPostSource::placeExactWindow(int first, const QStringList& ids)\n{\n    publishExactWindow(assignExactWindow(first, ids));\n}\n''',
)

replace_once(
    "sources/chat-area/ThreadPostSource.cpp",
    '''    const QVector<QString> before = postIds;\n    for (int offset = 0; offset < count; ++offset) {\n        const QString& id = ids.at(offset);\n        const int target = first + offset;\n        const int existing = postIndexes.value(id, -1);\n        if (existing >= 0 && existing != target) {\n            postIds[existing].clear();\n        }\n    }\n    for (int offset = 0; offset < count; ++offset) {\n        postIds[first + offset] = ids.at(offset);\n    }\n    rebuildIndex();\n\n    qCDebug(lcThreadTimelineTrace).nospace()\n        << "THREAD_PLACE_APPROX_DONE source=" << static_cast<const void*>(this)\n        << ' ' << slotSummary(postIds);\n\n    for (int index = 0; index < postIds.size(); ++index) {\n        if (before.at(index) != postIds.at(index) && !before.at(index).isEmpty()) {\n            emit itemsChanged(index, index);\n        }\n    }\n    emit rangeAvailable(first, last);\n''',
    '''    publishExactWindow(assignExactWindow(first, ids.mid(0, count)));\n\n    qCDebug(lcThreadTimelineTrace).nospace()\n        << "THREAD_PLACE_APPROX_DONE source=" << static_cast<const void*>(this)\n        << ' ' << slotSummary(postIds);\n''',
)

# Channel absolute-page placement keeps provisional-window policy locally, but
# delegates the pure exact identity mutation to IndexedPostSource.
replace_once(
    "sources/chat-area/ChannelPostSource.cpp",
    '''    bool touchesProvisionalIdentity = false;\n    bool mappingChanged = false;\n    QSet<int> concreteChanged;\n\n    for (int offset = 0; offset < count; ++offset) {\n        const QString& id = chronologicalIds.at(offset);\n        if (provisionalPostIds.contains(id)) {\n            touchesProvisionalIdentity = true;\n        }\n        const int existing = postIndexes.value(id, -1);\n        if (existing >= 0 && (existing < first || existing > last)\n            && !postIds.at(existing).isEmpty()) {\n            postIds[existing].clear();\n            concreteChanged.insert(existing);\n            mappingChanged = true;\n        }\n    }\n\n    for (int offset = 0; offset < count; ++offset) {\n        const int index = first + offset;\n        const QString& id = chronologicalIds.at(offset);\n        if (postIds.at(index) != id) {\n            if (!postIds.at(index).isEmpty()) {\n                concreteChanged.insert(index);\n            }\n            postIds[index] = id;\n            mappingChanged = true;\n        }\n        provisionalPostIds.remove(id);\n    }\n    rebuildIndex();\n''',
    '''    bool touchesProvisionalIdentity = false;\n    const QStringList pageIds = chronologicalIds.mid(0, count);\n    for (const QString& id : pageIds) {\n        touchesProvisionalIdentity = touchesProvisionalIdentity\n            || provisionalPostIds.contains(id);\n        provisionalPostIds.remove(id);\n    }\n    const ExactWindowMutation mutation = assignExactWindow(first, pageIds);\n''',
)
replace_once(
    "sources/chat-area/ChannelPostSource.cpp",
    '''    // Re-fetching an already known page must be a no-op. In particular, do not\n    // emit rangeAvailable/itemsChanged for identical identities: both signals\n    // schedule another synchronization, which used to clear request suppression\n    // and immediately ask for the same impossible oldest range again.\n    if (!mappingChanged) {\n        return;\n    }\n\n    for (int index : std::as_const(concreteChanged)) {\n        emit itemsChanged(index, index);\n    }\n    emit rangeAvailable(first, last);\n''',
    '''    // Re-fetching an already known page is deliberately a no-op. The shared\n    // publisher emits nothing unless the identity mapping actually changed.\n    publishExactWindow(mutation);\n''',
)

# ---------------------------------------------------------------------------
# Structural primitives.
# ---------------------------------------------------------------------------
replace_once(
    "sources/chat-area/ChannelPostSource.cpp",
    '''void ChannelPostSource::insertLogicalPrefix(int count)\n{\n    count = std::max(0, count);\n    if (count == 0) {\n        return;\n    }\n\n    if (provisionalWindow.isValid()) {\n        provisionalWindow.first += count;\n    }\n\n    QVector<QString> next;\n    next.reserve(count + static_cast<int>(postIds.size()));\n    for (int i = 0; i < count; ++i) {\n        next.push_back(QString());\n    }\n    for (const QString& id : std::as_const(postIds)) {\n        next.push_back(id);\n    }\n    postIds = std::move(next);\n    rebuildIndex();\n    emit itemsInserted(0, count);\n}\n''',
    '''void ChannelPostSource::insertLogicalPrefix(int count)\n{\n    count = std::max(0, count);\n    if (count == 0) {\n        return;\n    }\n\n    if (provisionalWindow.isValid()) {\n        provisionalWindow.first += count;\n    }\n    insertEmptyLogicalSlots(0, count);\n}\n''',
)
replace_once(
    "sources/chat-area/ChannelPostSource.cpp",
    '''    for (int index = first; index <= last; ++index) {\n        provisionalPostIds.remove(postIds.at(index));\n    }\n    for (int i = 0; i < count; ++i) {\n        postIds.removeAt(first);\n    }\n    rebuildIndex();\n    emit itemsRemoved(first, count);\n}\n''',
    '''    for (int index = first; index <= last; ++index) {\n        provisionalPostIds.remove(postIds.at(index));\n    }\n    eraseLogicalSlots(first, count);\n}\n''',
)
replace_once(
    "sources/chat-area/ChannelPostSource.cpp",
    '''void ChannelPostSource::prependDiscovered(const QStringList& chronologicalIds)\n{\n    if (chronologicalIds.isEmpty()) {\n        return;\n    }\n\n    QVector<QString> combined;\n    combined.reserve(static_cast<int>(chronologicalIds.size()) + static_cast<int>(postIds.size()));\n    for (const QString& id : chronologicalIds) {\n        combined.push_back(id);\n    }\n    for (const QString& id : std::as_const(postIds)) {\n        combined.push_back(id);\n    }\n\n    const int inserted = static_cast<int>(chronologicalIds.size());\n    postIds = std::move(combined);\n    rebuildIndex();\n    emit itemsInserted(0, inserted);\n    emit rangeAvailable(0, inserted - 1);\n}\n''',
    '''void ChannelPostSource::prependDiscovered(const QStringList& chronologicalIds)\n{\n    if (chronologicalIds.isEmpty()) {\n        return;\n    }\n\n    const int inserted = static_cast<int>(chronologicalIds.size());\n    insertEmptyLogicalSlots(0, inserted);\n    publishExactWindow(assignExactWindow(0, chronologicalIds));\n}\n''',
)
replace_once(
    "sources/chat-area/ChannelPostSource.cpp",
    '''    postIds.resize(count);\n    const int index = count - 1;\n    postIds[index] = post.id;\n    postIndexes.insert(post.id, index);\n    emit itemCountChanged(count);\n    emit rangeAvailable(index, index);\n''',
    '''    resizeLogicalTail(count);\n    const int index = count - 1;\n    publishExactWindow(assignExactWindow(index, QStringList { post.id }));\n''',
)

# Thread live reply: preserve the fixed double-counting rule, but let the base
# own tail resizing, relocation, index rebuilding and source signals.
replace_once(
    "sources/chat-area/ThreadPostSource.cpp",
    '''    if (count > oldCount) {\n        postIds.resize(count);\n    } else if (!metadataReservedTail) {\n        // Defensive fallback for a producer that delivers the live reply before\n        // root metadata has advanced. In that ordering the event itself is the\n        // only evidence that the logical thread grew.\n        count = oldCount + 1;\n        postIds.resize(count);\n    }\n    if (!postIds.isEmpty()) {\n        postIds[0] = rootId;\n    }\n    const int index = count - 1;\n    postIds[index] = post.id;\n    rebuildIndex();\n    qCDebug(lcThreadTimelineTrace).nospace()\n        << "THREAD_LIVE_APPEND source=" << static_cast<const void*>(this)\n        << " post=" << shortId(post.id)\n        << " index=" << index\n        << ' ' << slotSummary(postIds);\n    emit itemCountChanged(count);\n    emit rangeAvailable(index, index);\n''',
    '''    if (count > oldCount) {\n        resizeLogicalTail(count);\n    } else if (!metadataReservedTail) {\n        // Defensive fallback for a producer that delivers the live reply before\n        // root metadata has advanced. In that ordering the event itself is the\n        // only evidence that the logical thread grew.\n        count = oldCount + 1;\n        resizeLogicalTail(count);\n    }\n    const int index = count - 1;\n    publishExactWindow(assignExactWindow(index, QStringList { post.id }));\n    qCDebug(lcThreadTimelineTrace).nospace()\n        << "THREAD_LIVE_APPEND source=" << static_cast<const void*>(this)\n        << " post=" << shortId(post.id)\n        << " index=" << index\n        << ' ' << slotSummary(postIds);\n''',
)

# Remove concrete-source copies of rebuildIndex().
replace_once(
    "sources/chat-area/ChannelPostSource.cpp",
    '''void ChannelPostSource::rebuildIndex()\n{\n    postIndexes.clear();\n    for (int index = 0; index < postIds.size(); ++index) {\n        if (!postIds.at(index).isEmpty()) {\n            postIndexes.insert(postIds.at(index), index);\n        }\n    }\n}\n\n''',
    '',
)
replace_once(
    "sources/chat-area/ThreadPostSource.cpp",
    '''void ThreadPostSource::rebuildIndex()\n{\n    postIndexes.clear();\n    for (int index = 0; index < postIds.size(); ++index) {\n        if (!postIds.at(index).isEmpty()) {\n            postIndexes.insert(postIds.at(index), index);\n        }\n    }\n}\n\n''',
    '',
)

print("indexed post source refactor applied")
