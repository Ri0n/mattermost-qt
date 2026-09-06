from pathlib import Path


def replace_once(path, old, new):
    p = Path(path)
    text = p.read_text()
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected one match, found {count}")
    p.write_text(text.replace(old, new, 1))


# Source/view contract: identity and resident-body availability are independent.
replace_once(
    'sources/chat-area/AbstractPostSource.h',
    '    void rangeAvailable(int first, int last);\n    void itemsChanged(int first, int last);\n',
    '    void rangeAvailable(int first, int last);\n\n'
    '    /** Resident body availability changed without changing logical identity. */\n'
    '    void bodyAvailabilityChanged(int first, int last, bool available);\n\n'
    '    void itemsChanged(int first, int last);\n')

replace_once(
    'sources/chat-area/IndexedPostSource.cpp',
    '    : AbstractPostSource(parent)\n    , channel(channelInstance)\n{\n}\n',
    '    : AbstractPostSource(parent)\n    , channel(channelInstance)\n{\n'
    '    connect(&channel, &BackendChannel::onPostBodyAvailabilityChanged,\n'
    '            this, [this](const QString& postId, bool available) {\n'
    '        const int index = indexOfPost(postId);\n'
    '        if (index >= 0) {\n'
    '            emit bodyAvailabilityChanged(index, index, available);\n'
    '        }\n'
    '    });\n'
    '}\n')

replace_once(
    'sources/chat-area/ChatLogWidget.cpp',
    '''    sourceConnections.push_back(connect(postSource, &AbstractPostSource::rangeAvailable,\n                                        this, [this](int first, int last) {\n        qCDebug(lcTimelineTrace).nospace()\n            << "SOURCE_AVAILABLE list=" << static_cast<const void*>(this)\n            << " source=" << sourceName(postSource)\n            << " range=[" << first << ',' << last << ']';\n        setRangeAvailable(first, last, true);\n        restoreNavigationTarget();\n    }));\n''',
    '''    sourceConnections.push_back(connect(postSource, &AbstractPostSource::rangeAvailable,\n                                        this, [this](int first, int last) {\n        qCDebug(lcTimelineTrace).nospace()\n            << "SOURCE_AVAILABLE list=" << static_cast<const void*>(this)\n            << " source=" << sourceName(postSource)\n            << " range=[" << first << ',' << last << ']';\n        setRangeAvailable(first, last, true);\n        restoreNavigationTarget();\n    }));\n    sourceConnections.push_back(connect(postSource, &AbstractPostSource::bodyAvailabilityChanged,\n                                        this, [this](int first, int last, bool available) {\n        qCDebug(lcTimelineTrace).nospace()\n            << "SOURCE_BODY_AVAILABILITY list=" << static_cast<const void*>(this)\n            << " source=" << sourceName(postSource)\n            << " range=[" << first << ',' << last << ']'\n            << " available=" << available;\n        setRangeAvailable(first, last, available);\n        if (available) {\n            restoreNavigationTarget();\n        }\n    }));\n''')

# BackendChannel owns body lifetime and emits a dedicated body-only signal.
replace_once(
    'sources/backend/types/BackendChannel.h',
    '''\tBackendPost* addPost (const QJsonObject& postObject);\n\n\tvoid prependPosts''',
    '''\tBackendPost* addPost (const QJsonObject& postObject);\n\n\t/** Whether removing this resident body would leave no raw root dependency dangling. */\n\tbool canEvictPostBody(const QString& postId) const;\n\t/** Remove one resident body while preserving all external logical source identities. */\n\tbool evictPostBody(const QString& postId);\n\t/** Re-announce an insertion hidden by a selective quiet-ingest signal blocker. */\n\tvoid notifyPostBodyAvailable(const QString& postId);\n\n\tvoid prependPosts''')

replace_once(
    'sources/backend/types/BackendChannel.h',
    '''\t/** Called when a post is deleted. */\n\tvoid onPostDeleted (const QString& postId);\n\n\tvoid onUserTyping''',
    '''\t/** Called when a post is deleted. */\n\tvoid onPostDeleted (const QString& postId);\n\n\t/** Resident body appeared/disappeared while its semantic timeline ID may remain mapped. */\n\tvoid onPostBodyAvailabilityChanged(const QString& postId, bool available);\n\n\tvoid onUserTyping''')

replace_once(
    'sources/backend/types/BackendChannel.cpp',
    '''\tBackendPost* newPost = &posts.back ();\n\tpostIdToPost[newPost->id] = newPost;\n\n\tQString rootId = postObject.value("root_id").toString();\n\n\tif (!rootId.isEmpty()) {\n\t\trootIdAndPostList.push_back(QPair<QString,QString> (rootId, newPost->id));\n\t\tnewPost->hidden = true;\n\t\tBackendPost* rootPost = findPostById(rootId);\n\t\tif (rootPost) {\n\t\t\tqDebug() << rootPost->id <<  rootPost->message;\n\t\t\trootPost->has_thread = true;\n\t\t\t++rootPost->reply_count;\n\t\t\trootPost->last_reply_at = std::max(rootPost->last_reply_at, newPost->create_at);\n\t\t\temit onPostEdited(*rootPost);\n\t\t} else {\n\t\t\tmissingRootPostIds.insert(rootId);\n\t\t}\n\t}\n''',
    '''\tBackendPost* newPost = &posts.back ();\n\tpostIdToPost[newPost->id] = newPost;\n\tnotifyPostBodyAvailable(newPost->id);\n\n\tQString rootId = postObject.value("root_id").toString();\n\n\tif (!rootId.isEmpty()) {\n\t\tconst QPair<QString, QString> relation(rootId, newPost->id);\n\t\tconst bool knownReplyIdentity = rootIdAndPostList.contains(relation);\n\t\tif (!knownReplyIdentity) {\n\t\t\trootIdAndPostList.push_back(relation);\n\t\t}\n\t\tnewPost->hidden = true;\n\t\tBackendPost* rootPost = findPostById(rootId);\n\t\tif (rootPost) {\n\t\t\tqDebug() << rootPost->id <<  rootPost->message;\n\t\t\tbool rootChanged = !rootPost->has_thread;\n\t\t\trootPost->has_thread = true;\n\t\t\tif (!knownReplyIdentity) {\n\t\t\t\t++rootPost->reply_count;\n\t\t\t\trootChanged = true;\n\t\t\t}\n\t\t\tif (rootPost->last_reply_at < newPost->create_at) {\n\t\t\t\trootPost->last_reply_at = newPost->create_at;\n\t\t\t\trootChanged = true;\n\t\t\t}\n\t\t\tif (rootChanged) {\n\t\t\t\temit onPostEdited(*rootPost);\n\t\t\t}\n\t\t} else {\n\t\t\tmissingRootPostIds.insert(rootId);\n\t\t}\n\t}\n''')

replace_once(
    'sources/backend/types/BackendChannel.cpp',
    '''\tBackendPost* newPost = &*posts.emplace (position, postObject, storage);\n\tpostIdToPost[newPost->id] = newPost;\n\n\tcurrentChunk.postsToAdd.emplace_front (newPost);\n''',
    '''\tBackendPost* newPost = &*posts.emplace (position, postObject, storage);\n\tpostIdToPost[newPost->id] = newPost;\n\tnotifyPostBodyAvailable(newPost->id);\n\n\tcurrentChunk.postsToAdd.emplace_front (newPost);\n''')

replace_once(
    'sources/backend/types/BackendChannel.cpp',
    '''\tif (!rootId.isEmpty()) {\n\t\trootLinks.push_back(QPair<QString,QString> (rootId, newPost->id));\n\t\tnewPost->hidden = true;\n''',
    '''\tif (!rootId.isEmpty()) {\n\t\tconst QPair<QString, QString> relation(rootId, newPost->id);\n\t\tif (!rootLinks.contains(relation)) {\n\t\t\trootLinks.push_back(relation);\n\t\t}\n\t\tnewPost->hidden = true;\n''')

replace_once(
    'sources/backend/types/BackendChannel.cpp',
    '''BackendPost* BackendChannel::findPostById (QString postID)\n{\n''',
    '''bool BackendChannel::canEvictPostBody(const QString& postId) const\n{\n\tBackendPost* target = postIdToPost.value(postId, nullptr);\n\tif (!target) {\n\t\treturn false;\n\t}\n\n\t// Legacy thread presentation still carries a raw rootPost pointer. Root IDs\n\t// are the durable relationship, but until that pointer is removed a resident\n\t// reply conservatively pins its root body as well.\n\tfor (const BackendPost& candidate : posts) {\n\t\tif (&candidate == target) {\n\t\t\tcontinue;\n\t\t}\n\t\tif (candidate.rootPost == target || candidate.root_id == postId) {\n\t\t\treturn false;\n\t\t}\n\t}\n\treturn true;\n}\n\nbool BackendChannel::evictPostBody(const QString& postId)\n{\n\tif (!canEvictPostBody(postId)) {\n\t\treturn false;\n\t}\n\n\tauto body = std::find_if(posts.begin(), posts.end(), [&postId](const BackendPost& post) {\n\t\treturn post.id == postId;\n\t});\n\tif (body == posts.end()) {\n\t\treturn false;\n\t}\n\n\tpostIdToPost.remove(postId);\n\tposts.erase(body);\n\temit onPostBodyAvailabilityChanged(postId, false);\n\treturn true;\n}\n\nvoid BackendChannel::notifyPostBodyAvailable(const QString& postId)\n{\n\tif (!postId.isEmpty() && postIdToPost.contains(postId)) {\n\t\temit onPostBodyAvailabilityChanged(postId, true);\n\t}\n}\n\nBackendPost* BackendChannel::findPostById (QString postID)\n{\n''')

# Repository residency state + timer.
replace_once(
    'sources/backend/PostRepository.h',
    '#include <QPointer>\n#include <QStringList>\n',
    '#include <QPointer>\n#include <QStringList>\n#include <QTimer>\n')

replace_once(
    'sources/backend/PostRepository.h',
    '''    void pruneResidentObservations();\n    void releasePostLease(const QString& channelId, const QString& postId);\n''',
    '''    void pruneResidentObservations();\n    void initializeResidentMemory();\n    void noteResidentSnapshot(const QJsonObject& postObject, bool forceAdmission);\n    void scheduleResidentSweep();\n    void sweepResidentBodies();\n    void releasePostLease(const QString& channelId, const QString& postId);\n''')

replace_once(
    'sources/backend/PostRepository.h',
    '''    struct ResidentObservation {\n        quint64 sequence = 0;\n        qint64 touchedAt = 0;\n    };\n\n    QHash<QString, QList<JsonCallback>> inFlightGets;\n''',
    '''    struct ResidentObservation {\n        quint64 sequence = 0;\n        qint64 touchedAt = 0;\n    };\n    struct ResidentBodyState {\n        qint64 accountedBytes = 0;\n        qint64 touchedAt = 0;\n    };\n\n    QHash<QString, QList<JsonCallback>> inFlightGets;\n''')

replace_once(
    'sources/backend/PostRepository.h',
    '''    QHash<QString, ResidentObservation> residentObservations;\n    QHash<QString, int> residentLeaseCounts;\n    quint64 observationSequence = 0;\n''',
    '''    QHash<QString, ResidentObservation> residentObservations;\n    QHash<QString, ResidentBodyState> residentBodies;\n    QHash<QString, int> residentLeaseCounts;\n    qint64 residentAccountedBytes = 0;\n    QTimer residentSweepTimer;\n    bool residentSweepPending = false;\n    quint64 observationSequence = 0;\n''')

Path('sources/backend/PostRepositoryResidency.cpp').write_text(r'''#include "PostRepository.h"

#include <algorithm>
#include <utility>

#include <QDateTime>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QSet>
#include <QSettings>

#include "Backend.h"
#include "PostResidencyLease.h"
#include "Settings.h"
#include "Storage.h"
#include "types/BackendChannel.h"
#include "types/BackendPost.h"

namespace Mattermost {
namespace {

Q_LOGGING_CATEGORY(lcResidentPosts, "mattermost.cache.resident", QtWarningMsg)

constexpr qint64 MiB = 1024LL * 1024;
constexpr qint64 MinimumAccountedPostBytes = 4096;

QString residencyKey(const QString& channelId, const QString& postId)
{
    return channelId + QChar(0x1f) + postId;
}

qint64 configuredHardBytes()
{
    const qint64 mb = std::max<qint64>(1, QSettings().value(
        POST_CACHE_MEMORY_HARD_MB, POST_CACHE_MEMORY_HARD_MB_DEFAULT).toLongLong());
    return mb * MiB;
}

qint64 configuredTargetBytes()
{
    const qint64 hard = configuredHardBytes();
    const qint64 mb = std::max<qint64>(1, QSettings().value(
        POST_CACHE_MEMORY_TARGET_MB, POST_CACHE_MEMORY_TARGET_MB_DEFAULT).toLongLong());
    return std::min(hard, mb * MiB);
}

qint64 configuredTtlMs()
{
    const qint64 minutes = std::max<qint64>(1, QSettings().value(
        POST_CACHE_MEMORY_POST_TTL_MINUTES,
        POST_CACHE_MEMORY_POST_TTL_MINUTES_DEFAULT).toLongLong());
    return minutes * 60LL * 1000;
}

int configuredSweepMs()
{
    const qint64 seconds = std::max<qint64>(1, QSettings().value(
        POST_CACHE_MEMORY_SWEEP_SECONDS,
        POST_CACHE_MEMORY_SWEEP_SECONDS_DEFAULT).toLongLong());
    return static_cast<int>(std::min<qint64>(seconds * 1000,
                                             std::numeric_limits<int>::max()));
}

qint64 accountedJsonBytes(const QJsonObject& postObject)
{
    const qint64 wireBytes = QJsonDocument(postObject)
        .toJson(QJsonDocument::Compact).size();
    // The retained C++ object expands UTF-8 JSON into QStrings, containers,
    // attachments/reactions and allocator nodes. This is deliberately a stable
    // cache-accounting metric rather than a claim about process RSS.
    return std::max(MinimumAccountedPostBytes, 1024LL + wireBytes * 3);
}

qint64 fallbackAccountedBytes(const BackendPost& post)
{
    qint64 bytes = MinimumAccountedPostBytes;
    const auto addString = [&bytes](const QString& value) {
        bytes += static_cast<qint64>(value.capacity()) * sizeof(QChar) + 32;
    };

    addString(post.id);
    addString(post.user_id);
    addString(post.sender_name);
    addString(post.channel_id);
    addString(post.root_id);
    addString(post.parent_id);
    addString(post.original_id);
    addString(post.message);
    addString(post.type);
    addString(post.hashtags);
    addString(post.pending_post_id);
    for (const QString& participant : post.threadParticipantUserIds) {
        addString(participant);
    }
    for (const BackendFile& file : post.files) {
        bytes += sizeof(BackendFile) + 64;
        addString(file.id);
        addString(file.name);
        addString(file.mimeType);
        addString(file.extension);
    }
    for (const auto& reaction : post.reactions) {
        bytes += 64;
        for (const QString& user : reaction.second) {
            addString(user);
        }
    }
    if (post.props.isObject()) {
        bytes += QJsonDocument(post.props.toObject()).toJson(QJsonDocument::Compact).size() * 2LL;
    } else if (post.props.isArray()) {
        bytes += QJsonDocument(post.props.toArray()).toJson(QJsonDocument::Compact).size() * 2LL;
    }
    if (post.poll) {
        bytes += 2048;
    }
    return std::max(MinimumAccountedPostBytes, bytes);
}

struct EvictionCandidate {
    QPointer<BackendChannel> channel;
    QString postId;
    qint64 touchedAt = 0;
    qint64 accountedBytes = 0;
};

} // namespace

PostResidencyLease::PostResidencyLease(PostRepository* repositoryInstance,
                                       QString channelIdInstance,
                                       QString postIdInstance)
    : repository(repositoryInstance)
    , channelId(std::move(channelIdInstance))
    , postId(std::move(postIdInstance))
{
}

PostResidencyLease::~PostResidencyLease()
{
    reset();
}

PostResidencyLease::PostResidencyLease(PostResidencyLease&& other) noexcept
    : repository(other.repository)
    , channelId(std::move(other.channelId))
    , postId(std::move(other.postId))
{
    other.repository.clear();
    other.channelId.clear();
    other.postId.clear();
}

PostResidencyLease& PostResidencyLease::operator=(PostResidencyLease&& other) noexcept
{
    if (this == &other) {
        return *this;
    }
    reset();
    repository = other.repository;
    channelId = std::move(other.channelId);
    postId = std::move(other.postId);
    other.repository.clear();
    other.channelId.clear();
    other.postId.clear();
    return *this;
}

void PostResidencyLease::reset()
{
    if (repository) {
        repository->releasePostLease(channelId, postId);
    }
    repository.clear();
    channelId.clear();
    postId.clear();
}

void PostRepository::initializeResidentMemory()
{
    residentSweepTimer.setSingleShot(false);
    residentSweepTimer.setInterval(configuredSweepMs());
    connect(&residentSweepTimer, &QTimer::timeout,
            this, &PostRepository::sweepResidentBodies);
    residentSweepTimer.start();
}

void PostRepository::noteResidentSnapshot(const QJsonObject& postObject,
                                          bool forceAdmission)
{
    const QString postId = postObject.value(QStringLiteral("id")).toString();
    const QString channelId = postObject.value(QStringLiteral("channel_id")).toString();
    if (postId.isEmpty() || channelId.isEmpty()) {
        return;
    }

    BackendChannel* channel = backend.getStorage().getChannelById(channelId);
    if (!channel) {
        return;
    }

    const QString key = residencyKey(channelId, postId);
    auto it = residentBodies.find(key);
    if (!forceAdmission && it == residentBodies.end()
        && !shouldRetainChannelInMemory(channelId)) {
        return;
    }
    if (forceAdmission && !channel->postIdToPost.contains(postId)) {
        return;
    }

    const qint64 bytes = accountedJsonBytes(postObject);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (it == residentBodies.end()) {
        ResidentBodyState state;
        state.accountedBytes = bytes;
        state.touchedAt = now;
        residentBodies.insert(key, state);
        residentAccountedBytes += bytes;
    } else {
        residentAccountedBytes += bytes - it->accountedBytes;
        it->accountedBytes = bytes;
        it->touchedAt = now;
    }

    if (residentAccountedBytes > configuredHardBytes()) {
        scheduleResidentSweep();
    }
}

PostResidencyLease PostRepository::leasePost(const BackendPost& post)
{
    if (post.channel_id.isEmpty() || post.id.isEmpty()) {
        return {};
    }

    BackendChannel* channel = backend.getStorage().getChannelById(post.channel_id);
    if (!channel || channel->postIdToPost.value(post.id, nullptr) != &post) {
        // Pinned-post dialogs and transient notification objects can also create
        // PostWidgets. They own different BackendPost instances and must not pin
        // a same-ID resident channel body accidentally.
        return {};
    }

    const QString key = residencyKey(post.channel_id, post.id);
    auto state = residentBodies.find(key);
    if (state == residentBodies.end()) {
        ResidentBodyState fallback;
        fallback.accountedBytes = fallbackAccountedBytes(post);
        fallback.touchedAt = QDateTime::currentMSecsSinceEpoch();
        residentBodies.insert(key, fallback);
        residentAccountedBytes += fallback.accountedBytes;
    } else {
        state->touchedAt = QDateTime::currentMSecsSinceEpoch();
    }

    residentLeaseCounts[key] = residentLeaseCounts.value(key, 0) + 1;
    if (residentAccountedBytes > configuredHardBytes()) {
        scheduleResidentSweep();
    }
    return PostResidencyLease(this, post.channel_id, post.id);
}

bool PostRepository::isPostLeased(const QString& channelId,
                                  const QString& postId) const
{
    if (channelId.isEmpty() || postId.isEmpty()) {
        return false;
    }
    return residentLeaseCounts.value(residencyKey(channelId, postId), 0) > 0;
}

void PostRepository::releasePostLease(const QString& channelId,
                                      const QString& postId)
{
    const QString key = residencyKey(channelId, postId);
    auto it = residentLeaseCounts.find(key);
    if (it == residentLeaseCounts.end()) {
        return;
    }
    --(*it);
    if (*it <= 0) {
        residentLeaseCounts.erase(it);
    }
    if (residentAccountedBytes > configuredHardBytes()) {
        scheduleResidentSweep();
    }
}

void PostRepository::scheduleResidentSweep()
{
    if (residentSweepPending) {
        return;
    }
    residentSweepPending = true;
    QTimer::singleShot(0, this, [this] {
        residentSweepPending = false;
        sweepResidentBodies();
    });
}

void PostRepository::sweepResidentBodies()
{
    const int sweepMs = configuredSweepMs();
    if (residentSweepTimer.interval() != sweepMs) {
        residentSweepTimer.setInterval(sweepMs);
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 ttlMs = configuredTtlMs();
    const qint64 hardBytes = configuredHardBytes();
    const qint64 targetBytes = configuredTargetBytes();

    QVector<EvictionCandidate> candidates;
    QSet<QString> currentKeys;
    QSet<BackendChannel*> seenChannels;
    qint64 currentBytes = 0;

    const Storage& storage = backend.getStorage();
    for (auto channelIt = storage.channels.cbegin(); channelIt != storage.channels.cend(); ++channelIt) {
        BackendChannel* channel = channelIt.value();
        if (!channel || seenChannels.contains(channel)) {
            continue;
        }
        seenChannels.insert(channel);

        for (const BackendPost& post : channel->posts) {
            const QString key = residencyKey(channel->id, post.id);
            currentKeys.insert(key);
            auto state = residentBodies.find(key);
            if (state == residentBodies.end()) {
                ResidentBodyState fallback;
                fallback.accountedBytes = fallbackAccountedBytes(post);
                // Unknown legacy residents get a full TTL grace period instead
                // of being treated as ancient merely because accounting started.
                fallback.touchedAt = now;
                state = residentBodies.insert(key, fallback);
            }
            currentBytes += state->accountedBytes;

            if (residentLeaseCounts.value(key, 0) > 0
                || !channel->canEvictPostBody(post.id)) {
                continue;
            }
            candidates.push_back(EvictionCandidate {
                channel,
                post.id,
                state->touchedAt,
                state->accountedBytes,
            });
        }
    }

    for (auto it = residentBodies.begin(); it != residentBodies.end();) {
        if (!currentKeys.contains(it.key())) {
            it = residentBodies.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = residentLeaseCounts.begin(); it != residentLeaseCounts.end();) {
        if (!currentKeys.contains(it.key())) {
            it = residentLeaseCounts.erase(it);
        } else {
            ++it;
        }
    }
    residentAccountedBytes = currentBytes;

    std::sort(candidates.begin(), candidates.end(), [](const EvictionCandidate& lhs,
                                                        const EvictionCandidate& rhs) {
        if (lhs.touchedAt != rhs.touchedAt) {
            return lhs.touchedAt < rhs.touchedAt;
        }
        return lhs.postId < rhs.postId;
    });

    const auto evict = [this, &currentBytes](const EvictionCandidate& candidate) {
        if (!candidate.channel
            || isPostLeased(candidate.channel->id, candidate.postId)
            || !candidate.channel->canEvictPostBody(candidate.postId)
            || !candidate.channel->evictPostBody(candidate.postId)) {
            return false;
        }
        const QString key = residencyKey(candidate.channel->id, candidate.postId);
        auto state = residentBodies.find(key);
        const qint64 bytes = state == residentBodies.end()
            ? candidate.accountedBytes : state->accountedBytes;
        currentBytes = std::max<qint64>(0, currentBytes - bytes);
        residentBodies.remove(key);
        residentLeaseCounts.remove(key);
        return true;
    };

    // TTL is independent from pressure: an unleased body not touched for the
    // configured period is cold even when plenty of budget remains.
    for (const EvictionCandidate& candidate : std::as_const(candidates)) {
        if (now - candidate.touchedAt >= ttlMs) {
            evict(candidate);
        }
    }

    // The hard limit is an accounted cap. Once crossed, evict oldest unleased
    // bodies until the lower pressure target is reached, creating hysteresis.
    if (currentBytes > hardBytes) {
        for (const EvictionCandidate& candidate : std::as_const(candidates)) {
            if (currentBytes <= targetBytes) {
                break;
            }
            evict(candidate);
        }
    }

    residentAccountedBytes = currentBytes;
    if (residentAccountedBytes > hardBytes) {
        qCWarning(lcResidentPosts).nospace()
            << "resident post cache remains above hard limit: accounted="
            << residentAccountedBytes << " hard=" << hardBytes
            << " target=" << targetBytes
            << " (remaining bodies are leased or dependency-pinned)";
    }
}

} // namespace Mattermost
''')

# Initialize/account accepted snapshots and preserve body availability through quiet ingest.
replace_once(
    'sources/backend/PostRepository.cpp',
    '''    connect(&httpConnector, &HTTPConnector::onHttpError,\n            &backend, &Backend::onHttpError);\n}\n''',
    '''    connect(&httpConnector, &HTTPConnector::onHttpError,\n            &backend, &Backend::onHttpError);\n    initializeResidentMemory();\n}\n''')

replace_once(
    'sources/backend/PostRepository.cpp',
    '''    const QString channelId = postObject.value(QStringLiteral("channel_id")).toString();\n    if (shouldCacheChannelOnDisk(channelId)) {\n''',
    '''    const QString channelId = postObject.value(QStringLiteral("channel_id")).toString();\n    noteResidentSnapshot(postObject, false);\n    if (shouldCacheChannelOnDisk(channelId)) {\n''')

replace_once(
    'sources/backend/PostRepository.cpp',
    '''    if (acceptedPosts.isEmpty()) {\n        pruneResidentObservations();\n        return;\n    }\n\n    // Mark before mutation.''',
    '''    if (acceptedPosts.isEmpty()) {\n        pruneResidentObservations();\n        return;\n    }\n\n    QStringList newlyResident;\n    for (auto it = acceptedPosts.constBegin(); it != acceptedPosts.constEnd(); ++it) {\n        if (!channel.postIdToPost.contains(it.key())) {\n            newlyResident.push_back(it.key());\n        }\n    }\n\n    // Mark before mutation.''')

replace_once(
    'sources/backend/PostRepository.cpp',
    '''    if (quiet) {\n        const QSignalBlocker blocker(&channel);\n        channel.mergePostContext(newestFirst, acceptedPosts);\n    } else {\n        channel.mergePostContext(newestFirst, acceptedPosts);\n    }\n    pruneResidentObservations();\n}\n\nvoid PostRepository::ingestCached''',
    '''    if (quiet) {\n        {\n            const QSignalBlocker blocker(&channel);\n            channel.mergePostContext(newestFirst, acceptedPosts);\n        }\n        for (const QString& postId : newlyResident) {\n            channel.notifyPostBodyAvailable(postId);\n        }\n    } else {\n        channel.mergePostContext(newestFirst, acceptedPosts);\n    }\n    for (auto it = acceptedPosts.constBegin(); it != acceptedPosts.constEnd(); ++it) {\n        noteResidentSnapshot(it->toObject(), true);\n    }\n    pruneResidentObservations();\n}\n\nvoid PostRepository::ingestCached''')

replace_once(
    'sources/backend/PostRepository.cpp',
    '''    if (acceptedPosts.isEmpty()) {\n        return;\n    }\n\n    const QStringList chronological = allChronologicalOrder(acceptedPosts);\n''',
    '''    if (acceptedPosts.isEmpty()) {\n        return;\n    }\n\n    const QStringList newlyResident = acceptedPosts.keys();\n    const QStringList chronological = allChronologicalOrder(acceptedPosts);\n''')

replace_once(
    'sources/backend/PostRepository.cpp',
    '''    if (quiet) {\n        const QSignalBlocker blocker(&channel);\n        channel.mergePostContext(newestFirst, acceptedPosts);\n    } else {\n        channel.mergePostContext(newestFirst, acceptedPosts);\n    }\n}\n\nvoid PostRepository::noteResidentObservation''',
    '''    if (quiet) {\n        {\n            const QSignalBlocker blocker(&channel);\n            channel.mergePostContext(newestFirst, acceptedPosts);\n        }\n        for (const QString& postId : newlyResident) {\n            channel.notifyPostBodyAvailable(postId);\n        }\n    } else {\n        channel.mergePostContext(newestFirst, acceptedPosts);\n    }\n    for (auto it = acceptedPosts.constBegin(); it != acceptedPosts.constEnd(); ++it) {\n        noteResidentSnapshot(it->toObject(), true);\n    }\n}\n\nvoid PostRepository::noteResidentObservation''')

# Thread source: root lease + body-aware cursor readiness.
replace_once(
    'sources/chat-area/ThreadPostSource.h',
    '#include "IndexedPostSource.h"\n',
    '#include "IndexedPostSource.h"\n#include "backend/PostResidencyLease.h"\n')

replace_once(
    'sources/chat-area/ThreadPostSource.h',
    '''    bool isAuthoritativeIndex(int index) const;\n    void pruneProvisionalPostIds();\n''',
    '''    bool isAuthoritativeIndex(int index) const;\n    bool isCursorReadyIndex(int index) const;\n    void pruneProvisionalPostIds();\n''')

replace_once(
    'sources/chat-area/ThreadPostSource.h',
    '''    Backend& backend;\n    QString rootId;\n    QSet<QString> provisionalPostIds;\n''',
    '''    Backend& backend;\n    QString rootId;\n    PostResidencyLease rootResidencyLease;\n    QSet<QString> provisionalPostIds;\n''')

replace_once(
    'sources/chat-area/ThreadPostSource.cpp',
    '''    if (!postIds.isEmpty() && rootPost()) {\n        postIds[0] = rootId;\n    }\n    seedCachedPosts();\n\n    BackendPost* root = rootPost();\n''',
    '''    if (!postIds.isEmpty() && rootPost()) {\n        postIds[0] = rootId;\n    }\n    seedCachedPosts();\n\n    BackendPost* root = rootPost();\n    if (root) {\n        rootResidencyLease = PostTimelineService::instance(backend).leasePost(*root);\n    }\n''')

replace_once(
    'sources/chat-area/ThreadPostSource.cpp',
    '''    for (int index = requestedFirst; index <= requestedLast; ++index) {\n        if (isAuthoritativeIndex(index)) {\n            continue;\n        }\n        if (firstMissing < 0) {\n            firstMissing = index;\n        }\n        lastMissing = index;\n    }\n\n    // Once either side of a gap is known''',
    '''    for (int index = requestedFirst; index <= requestedLast; ++index) {\n        if (isCursorReadyIndex(index)) {\n            continue;\n        }\n        if (firstMissing < 0) {\n            firstMissing = index;\n        }\n        lastMissing = index;\n    }\n    if (firstMissing < 0) {\n        emit rangeRequestFinished(first, last);\n        return;\n    }\n\n    // Once either side of a gap is known''')

replace_once(
    'sources/chat-area/ThreadPostSource.cpp',
    '    if (firstMissing > 0 && isAuthoritativeIndex(firstMissing - 1)) {\n',
    '    if (firstMissing > 0 && isCursorReadyIndex(firstMissing - 1)) {\n')

replace_once(
    'sources/chat-area/ThreadPostSource.cpp',
    '''        && lastMissing + 1 < static_cast<int>(postIds.size())\n        && isAuthoritativeIndex(lastMissing + 1)) {\n''',
    '''        && lastMissing + 1 < static_cast<int>(postIds.size())\n        && isCursorReadyIndex(lastMissing + 1)) {\n''')

replace_once(
    'sources/chat-area/ThreadPostSource.cpp',
    '''bool ThreadPostSource::isAuthoritativeIndex(int index) const\n{\n    if (index < 0 || index >= static_cast<int>(postIds.size())) {\n        return false;\n    }\n    const QString& id = postIds.at(index);\n    return !id.isEmpty() && !provisionalPostIds.contains(id);\n}\n\nvoid ThreadPostSource::pruneProvisionalPostIds()\n''',
    '''bool ThreadPostSource::isAuthoritativeIndex(int index) const\n{\n    if (index < 0 || index >= static_cast<int>(postIds.size())) {\n        return false;\n    }\n    const QString& id = postIds.at(index);\n    return !id.isEmpty() && !provisionalPostIds.contains(id);\n}\n\nbool ThreadPostSource::isCursorReadyIndex(int index) const\n{\n    // Identity authority survives body eviction, but a Mattermost compound\n    // cursor also needs the resident create_at value. Do not confuse those two\n    // states or a known-but-evicted ID becomes an unusable cursor anchor.\n    return isAuthoritativeIndex(index) && isAvailable(index);\n}\n\nvoid ThreadPostSource::pruneProvisionalPostIds()\n''')

# Quoted-reply composer keeps its source body until fallback construction/send completes.
replace_once(
    'sources/chat-area/QuotedReplyController.h',
    '#include <functional>\n',
    '#include <functional>\n\n#include "backend/PostResidencyLease.h"\n')

replace_once(
    'sources/chat-area/QuotedReplyController.h',
    '''    OutgoingPostCreator* editor = nullptr;\n    Mode mode = Mode::None;\n''',
    '''    OutgoingPostCreator* editor = nullptr;\n    PostResidencyLease replyResidencyLease;\n    Mode mode = Mode::None;\n''')

replace_once(
    'sources/chat-area/QuotedReplyController.cpp',
    '#include "backend/PostProps.h"\n',
    '#include "backend/PostProps.h"\n#include "backend/PostRepository.h"\n')

replace_once(
    'sources/chat-area/QuotedReplyController.cpp',
    '''    mode = Mode::Reply;\n    preview->setActivatedCallback({});\n    preview->setPost(post);\n''',
    '''    mode = Mode::Reply;\n    replyResidencyLease = PostRepository::instance(area.getBackend()).leasePost(post);\n    preview->setActivatedCallback({});\n    preview->setPost(post);\n''')

replace_once(
    'sources/chat-area/QuotedReplyController.cpp',
    '''    editor->setProperty(PostProps::ReplyToPostId, QString());\n    if (mode == Mode::Reply) {\n        mode = Mode::None;\n    }\n''',
    '''    editor->setProperty(PostProps::ReplyToPostId, QString());\n    replyResidencyLease.reset();\n    if (mode == Mode::Reply) {\n        mode = Mode::None;\n    }\n''')

replace_once(
    'sources/chat-area/QuotedReplyController.cpp',
    '''        if (!hasReply && mode == Mode::Reply) {\n            mode = Mode::None;\n        }\n        syncVisibility();\n''',
    '''        if (!hasReply && mode == Mode::Reply) {\n            mode = Mode::None;\n            replyResidencyLease.reset();\n        }\n        syncVisibility();\n''')

replace_once(
    'sources/chat-area/QuotedReplyController.cpp',
    '''        if (editing) {\n            mode = Mode::Editing;\n            preview->setActivatedCallback({});\n''',
    '''        if (editing) {\n            mode = Mode::Editing;\n            replyResidencyLease.reset();\n            preview->setActivatedCallback({});\n''')

# Runtime documentation now describes an enabled limiter and the distinct body signal.
replace_once(
    'docs/post-cache-runtime.md',
    'The planned resident cache deliberately keeps logical source IDs after their `BackendPost` body is\nevicted. Therefore two state changes must remain independent:\n',
    'The resident cache keeps logical source IDs after their `BackendPost` body is evicted. Therefore two\nstate changes remain independent:\n')

replace_once(
    'docs/post-cache-runtime.md',
    '''Current no-op suppression for an identical authoritative page is correct while every mapped ID is\nresident. Phase 3 must add an explicit rematerialization availability path rather than making an\nidentical page pretend that its identity mapping changed. This preserves both request-loop protection\nand safe body eviction.\n''',
    '''An identical authoritative page can leave identity mapping unchanged while restoring missing bodies.\n`BackendChannel::onPostBodyAvailabilityChanged` -> `IndexedPostSource::bodyAvailabilityChanged` is the\nseparate rematerialization path: `LongListWidget` clears/materializes body availability without\npretending that the source identity mapping changed. This preserves both request-loop protection and\nsafe body eviction.\n''')

replace_once(
    'docs/post-cache-runtime.md',
    'The resident-memory limiter is **not fully enabled yet**. The target contract is:\n',
    'The resident-memory limiter is enabled with the following contract:\n')

replace_once(
    'docs/post-cache-runtime.md',
    '''Before erasing resident `BackendPost` objects, raw-pointer lifetimes must be audited. In particular,\n`BackendPost::rootPost` cannot serve as a durable ownership relation; `root_id` must remain the stable\nidentity reference.\n''',
    '''`PostResidencyLease` is the move-only RAII pin for retained raw-reference/semantic-body dependencies.\nMaterialized `PostWidget`s, active edit/reply composer contexts and active thread roots hold leases.\nPinned-dialog copies do not accidentally pin a same-ID resident body because lease acquisition verifies\nobject ownership in `BackendChannel::postIdToPost`. The legacy `BackendPost::rootPost` relationship is\nstill handled conservatively: a root is not evictable while any resident reply names that root. The\ndurable relationship remains `root_id`, and removing the raw root pointer is still a later cleanup.\n\nThe 30-second sweeper removes unleased bodies after the 5-minute idle TTL. It also enforces a 500 MiB\naccounted hard limit with a 400 MiB pressure target. Accounted bytes use a stable approximation derived\nfrom compact raw JSON (or a structural fallback for legacy residents); this is intentionally cache\naccounting, not process RSS. Crossing the hard limit schedules an immediate sweep, and leases/dependency\npins are never violated even when that means temporarily remaining above the configured cap.\n''')

print('resident eviction and rematerialization lifecycle integrated')
