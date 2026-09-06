#include "IndexedPostSource.h"

#include <algorithm>

#include "backend/types/BackendChannel.h"

namespace Mattermost {

IndexedPostSource::IndexedPostSource(BackendChannel& channelInstance, QObject* parent)
    : AbstractPostSource(parent)
    , channel(channelInstance)
{
}

bool IndexedPostSource::isAvailable(int index) const
{
    return index >= 0 && index < postIds.size() && !postIds.at(index).isEmpty()
        && channel.postIdToPost.contains(postIds.at(index));
}

BackendPost* IndexedPostSource::postAt(int index) const
{
    if (!isAvailable(index)) {
        return nullptr;
    }
    return channel.postIdToPost.value(postIds.at(index), nullptr);
}

int IndexedPostSource::indexOfPost(const QString& postId) const
{
    return postIndexes.value(postId, -1);
}

IndexedPostSource::ExactWindowMutation IndexedPostSource::assignExactWindow(
    int first,
    const QStringList& ids)
{
    ExactWindowMutation mutation;
    if (postIds.isEmpty() || ids.isEmpty()) {
        return mutation;
    }

    first = std::max(0, std::min(first, static_cast<int>(postIds.size()) - 1));
    const int count = std::min(static_cast<int>(ids.size()),
                               static_cast<int>(postIds.size()) - first);
    if (count <= 0) {
        return mutation;
    }

    mutation.first = first;
    mutation.last = first + count - 1;

    // First remove old occurrences of identities that are moving into this
    // exact window. Doing this as a separate pass also makes swaps safe.
    for (int offset = 0; offset < count; ++offset) {
        const QString& id = ids.at(offset);
        if (id.isEmpty()) {
            continue;
        }
        const int target = first + offset;
        const int existing = postIndexes.value(id, -1);
        if (existing >= 0 && existing != target && !postIds.at(existing).isEmpty()) {
            postIds[existing].clear();
            mutation.concreteChanged.insert(existing);
            mutation.mappingChanged = true;
        }
    }

    for (int offset = 0; offset < count; ++offset) {
        const QString& id = ids.at(offset);
        if (id.isEmpty()) {
            continue;
        }
        const int target = first + offset;
        if (postIds.at(target) == id) {
            continue;
        }
        if (!postIds.at(target).isEmpty()) {
            mutation.concreteChanged.insert(target);
        }
        postIds[target] = id;
        mutation.mappingChanged = true;
    }

    if (mutation.mappingChanged) {
        rebuildIndex();
    }
    return mutation;
}

void IndexedPostSource::publishExactWindow(const ExactWindowMutation& mutation)
{
    if (!mutation.isValid() || !mutation.mappingChanged) {
        return;
    }

    for (int index : mutation.concreteChanged) {
        emit itemsChanged(index, index);
    }
    emit rangeAvailable(mutation.first, mutation.last);
}

bool IndexedPostSource::resizeLogicalTail(int count)
{
    count = std::max(0, count);
    if (count == postIds.size()) {
        return false;
    }

    postIds.resize(count);
    rebuildIndex();
    emit itemCountChanged(count);
    return true;
}

void IndexedPostSource::insertEmptyLogicalSlots(int first, int count)
{
    first = std::max(0, std::min(first, static_cast<int>(postIds.size())));
    count = std::max(0, count);
    if (count == 0) {
        return;
    }

    postIds.insert(first, count, QString());
    rebuildIndex();
    emit itemsInserted(first, count);
}

void IndexedPostSource::eraseLogicalSlots(int first, int count)
{
    first = std::max(0, std::min(first, static_cast<int>(postIds.size())));
    count = std::max(0, std::min(count, static_cast<int>(postIds.size()) - first));
    if (count == 0) {
        return;
    }

    for (int i = 0; i < count; ++i) {
        postIds.removeAt(first);
    }
    rebuildIndex();
    emit itemsRemoved(first, count);
}

void IndexedPostSource::rebuildIndex()
{
    postIndexes.clear();
    for (int index = 0; index < postIds.size(); ++index) {
        if (!postIds.at(index).isEmpty()) {
            postIndexes.insert(postIds.at(index), index);
        }
    }
}

} // namespace Mattermost
