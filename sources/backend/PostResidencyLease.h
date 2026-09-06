#pragma once

#include <QPointer>
#include <QString>

namespace Mattermost {

class PostRepository;

/**
 * Move-only lifetime pin for one resident BackendPost body.
 *
 * Logical source identity is deliberately not leased: a source may keep a
 * post ID after the body is evicted. The lease only says that a live raw
 * BackendPost reference/pointer exists and therefore the body must not move or
 * disappear until this object is released.
 *
 * PostRepository only issues a lease when the referenced object is the actual
 * BackendChannel-owned resident body. Transient WebSocket objects and pinned
 * dialog copies therefore cannot accidentally pin a same-ID channel object.
 * Releasing the last lease starts that body's idle-TTL window; memory pressure
 * may still evict an unleased body immediately when the hard cap is exceeded.
 */
class PostResidencyLease
{
public:
    PostResidencyLease() = default;
    ~PostResidencyLease();

    PostResidencyLease(const PostResidencyLease&) = delete;
    PostResidencyLease& operator=(const PostResidencyLease&) = delete;

    PostResidencyLease(PostResidencyLease&& other) noexcept;
    PostResidencyLease& operator=(PostResidencyLease&& other) noexcept;

    explicit operator bool() const { return !repository.isNull(); }
    void reset();

private:
    friend class PostRepository;
    PostResidencyLease(PostRepository* repository,
                       QString channelId,
                       QString postId);

    QPointer<PostRepository> repository;
    QString channelId;
    QString postId;
};

} // namespace Mattermost
