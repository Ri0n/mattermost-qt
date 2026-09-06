#include "PostRepository.h"

#include <algorithm>
#include <limits>
#include <utility>

#include <QDateTime>
#include <QJsonArray>
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
        auto state = residentBodies.find(key);
        if (state != residentBodies.end()) {
            // Idle TTL starts when the last active consumer lets go, not when a
            // long-lived widget/edit session first acquired its lease. Pressure
            // eviction remains independent of TTL.
            state->touchedAt = QDateTime::currentMSecsSinceEpoch();
        }
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
