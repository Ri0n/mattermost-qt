#include "PostRepository.h"

#include <algorithm>
#include <utility>

#include "PostResidencyLease.h"
#include "types/BackendPost.h"

namespace Mattermost {
namespace {

QString residencyKey(const QString& channelId, const QString& postId)
{
    return channelId + QChar(0x1f) + postId;
}

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

PostResidencyLease PostRepository::leasePost(const BackendPost& post)
{
    if (post.channel_id.isEmpty() || post.id.isEmpty()) {
        return {};
    }

    const QString key = residencyKey(post.channel_id, post.id);
    residentLeaseCounts[key] = residentLeaseCounts.value(key, 0) + 1;
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
}

} // namespace Mattermost
