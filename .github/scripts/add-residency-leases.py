from pathlib import Path


def replace_once(path, old, new):
    p = Path(path)
    text = p.read_text()
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected one match, found {count}")
    p.write_text(text.replace(old, new, 1))


Path('sources/backend/PostResidencyLease.h').write_text(r'''#pragma once

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
''')

Path('sources/backend/PostRepositoryResidency.cpp').write_text(r'''#include "PostRepository.h"

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
''')

replace_once(
    'sources/backend/PostRepository.h',
    '#include "PostCacheService.h"\n',
    '#include "PostCacheService.h"\n#include "PostResidencyLease.h"\n')
replace_once(
    'sources/backend/PostRepository.h',
    'class BackendChannel;\n',
    'class BackendChannel;\nclass BackendPost;\n')
replace_once(
    'sources/backend/PostRepository.h',
    '''    /** Whether post bodies from this channel may remain materialized in RAM. */\n    bool shouldRetainChannelInMemory(const QString& channelId) const;\n\n    /** Whether full post payloads from this channel are worth persisting. */\n''',
    '''    /** Whether post bodies from this channel may remain materialized in RAM. */\n    bool shouldRetainChannelInMemory(const QString& channelId) const;\n\n    /** Pin a resident post body while a raw BackendPost reference is retained. */\n    PostResidencyLease leasePost(const BackendPost& post);\n\n    /** True while at least one explicit raw-reference lease protects the body. */\n    bool isPostLeased(const QString& channelId, const QString& postId) const;\n\n    /** Whether full post payloads from this channel are worth persisting. */\n''')
replace_once(
    'sources/backend/PostRepository.h',
    '''    void noteResidentPostObservation(const QString& postId, quint64 observation);\n    void pruneResidentObservations();\n\n    Backend& backend;\n''',
    '''    void noteResidentPostObservation(const QString& postId, quint64 observation);\n    void pruneResidentObservations();\n    void releasePostLease(const QString& channelId, const QString& postId);\n\n    friend class PostResidencyLease;\n\n    Backend& backend;\n''')
replace_once(
    'sources/backend/PostRepository.h',
    '''    QHash<QString, qint64> channelOpenedAtByAccount;\n    QHash<QString, ResidentObservation> residentObservations;\n    quint64 observationSequence = 0;\n''',
    '''    QHash<QString, qint64> channelOpenedAtByAccount;\n    QHash<QString, ResidentObservation> residentObservations;\n    QHash<QString, int> residentLeaseCounts;\n    quint64 observationSequence = 0;\n''')

# Every PostWidget owns a lease for exactly the lifetime of its raw BackendPost&.
replace_once(
    'sources/chat-area/post/PostWidget.h',
    '#include "backend/types/BackendPost.h"\n',
    '#include "backend/PostResidencyLease.h"\n#include "backend/types/BackendPost.h"\n')
replace_once(
    'sources/chat-area/post/PostWidget.h',
    '''    Backend&                            backend;\n    Ui::PostWidget*\t\t\t\t\t\tui;\n''',
    '''    Backend&                            backend;\n    PostResidencyLease                 residencyLease;\n    Ui::PostWidget*\t\t\t\t\t\tui;\n''')
replace_once(
    'sources/chat-area/post/PostWidget.cpp',
    '''    , threadButton(nullptr)\n    , backend(backend)\n    , ui(new Ui::PostWidget)\n''',
    '''    , threadButton(nullptr)\n    , backend(backend)\n    , residencyLease(PostRepository::instance(backend).leasePost(post))\n    , ui(new Ui::PostWidget)\n''')

# The editor keeps a second lease because its raw pointer can outlive the
# materialized PostWidget after the row scrolls out of the widget budget.
replace_once(
    'sources/chat-area/outgoing-post/OutgoingPostCreator.h',
    '#include "MessageTextEditWidget.h"\n',
    '#include "MessageTextEditWidget.h"\n#include "backend/PostResidencyLease.h"\n')
replace_once(
    'sources/chat-area/outgoing-post/OutgoingPostCreator.h',
    '''\tconst BackendPost*\t\t\t\t\tpostToEdit;\n\tOutgoingAttachmentList*\t\t\t\tattachmentList;\n''',
    '''\tconst BackendPost*\t\t\t\t\tpostToEdit;\n\tPostResidencyLease\t\t\t\t\teditResidencyLease;\n\tOutgoingAttachmentList*\t\t\t\tattachmentList;\n''')
replace_once(
    'sources/chat-area/outgoing-post/OutgoingPostCreator.h',
    '''\t\tclear();\n\t\tpostToEdit = nullptr;\n\t\tsetEditingVisual(false);\n''',
    '''\t\tclear();\n\t\tpostToEdit = nullptr;\n\t\teditResidencyLease.reset();\n\t\tsetEditingVisual(false);\n''')
replace_once(
    'sources/chat-area/outgoing-post/OutgoingPostCreator.cpp',
    '#include "backend/PostProps.h"\n',
    '#include "backend/PostProps.h"\n#include "backend/PostRepository.h"\n')
replace_once(
    'sources/chat-area/outgoing-post/OutgoingPostCreator.cpp',
    '''\t\tclear();\n\t\tpostToEdit = nullptr;\n\t\tsetEditingVisual(false);\n''',
    '''\t\tclear();\n\t\tpostToEdit = nullptr;\n\t\teditResidencyLease.reset();\n\t\tsetEditingVisual(false);\n''')
replace_once(
    'sources/chat-area/outgoing-post/OutgoingPostCreator.cpp',
    '''\tsetProperty(PostProps::ReplyToPostId, QString());\n\tpostToEdit = &post;\n\tsetText(QuotedReplyFormat::stripFallback(post.message));\n''',
    '''\tsetProperty(PostProps::ReplyToPostId, QString());\n\tpostToEdit = &post;\n\teditResidencyLease = PostRepository::instance(*backend).leasePost(post);\n\tsetText(QuotedReplyFormat::stripFallback(post.message));\n''')
replace_once(
    'sources/chat-area/outgoing-post/OutgoingPostCreator.cpp',
    '''\toutgoingPostData.reset();\n\tsendFailed = false;\n''',
    '''\toutgoingPostData.reset();\n\teditResidencyLease.reset();\n\tsendFailed = false;\n''')

# Document what is protected now and what still blocks actual eviction.
replace_once(
    'docs/post-cache-runtime.md',
    '''A lease means: "some live UI/service object owns a raw `BackendPost&`/pointer and the body must not\nmove or disappear."\n''',
    '''A lease means: "some live UI/service object owns a raw `BackendPost&`/pointer and the body must not\nmove or disappear." `PostResidencyLease` now implements this contract as a move-only RAII pin in\n`PostRepository`. Every `PostWidget` holds one for its full widget lifetime, and the outgoing composer\nholds an independent edit-session lease because `postToEdit` can outlive the materialized row while an\nedit request is pending or retryable.\n''')
replace_once(
    'docs/post-cache-runtime.md',
    '''The eviction algorithm may only consider bodies with zero leases.\n''',
    '''The eviction algorithm may only consider bodies with zero explicit leases. A root post also remains\nimplicitly non-evictable while any resident reply still names it as `root_id`; this protects the legacy\n`BackendPost::rootPost` raw relationship until that relationship is converted to semantic identity.\n''')

print('resident raw-reference leases integrated')
