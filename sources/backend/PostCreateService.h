#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>

namespace Mattermost {

class Backend;
class BackendChannel;

/**
 * Lightweight adapter used by the composer for metadata-capable post creation.
 * Backend remains the sole owner of the HTTP post creation path.
 */
class PostCreateService final
{
public:
    static PostCreateService instance(Backend& backend);

    void createPost(BackendChannel& channel,
                    const QString& message,
                    const QList<QString>& attachments,
                    const QString& rootId = QString(),
                    const QJsonObject& props = QJsonObject());

private:
    explicit PostCreateService(Backend& backend);

    Backend& backend;
};

} // namespace Mattermost
