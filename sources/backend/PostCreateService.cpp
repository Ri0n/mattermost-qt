#include "PostCreateService.h"

#include "Backend.h"
#include "backend/types/BackendChannel.h"

namespace Mattermost {

PostCreateService PostCreateService::instance(Backend& backend)
{
    return PostCreateService(backend);
}

PostCreateService::PostCreateService(Backend& sourceBackend)
    : backend(sourceBackend)
{
}

void PostCreateService::createPost(BackendChannel& channel,
                                   const QString& message,
                                   const QList<QString>& attachments,
                                   const QString& rootId,
                                   const QJsonObject& props)
{
    backend.addPost(channel, message, attachments, rootId, props);
}

} // namespace Mattermost
