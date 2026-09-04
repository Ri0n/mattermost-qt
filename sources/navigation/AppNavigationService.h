#pragma once

#include <cstdint>
#include <functional>

#include <QObject>
#include <QStringList>
#include <QUrl>

#include "backend/HTTPConnector.h"

namespace Mattermost {

class Backend;
class BackendChannel;

class AppNavigationService final : public QObject
{
    Q_OBJECT
public:
    using NavigationCallback = std::function<void(bool)>;

    static AppNavigationService& instance(Backend& backend);

    void openUrl(const QUrl& url);
    void openChannel(const QString& channelId);
    void openPost(const QString& postId);
    void openThreadAtLastViewed(const QString& channelId,
                                const QString& rootId,
                                uint64_t lastViewedAt,
                                const QString& fallbackPostId = QString(),
                                NavigationCallback callback = {});

signals:
    void channelRequested(const QString& channelId,
                          const QString& postId,
                          const QString& rootId,
                          const QStringList& contextPostIds,
                          bool reachedOldest,
                          bool reachedNewest);

private:
    explicit AppNavigationService(Backend& backend);

    bool isLocalUrl(const QUrl& url) const;
    BackendChannel* findChannel(const QString& teamName,
                                const QString& channelName) const;
    BackendChannel* findPostChannel(const QString& postId) const;
    void openPostInChannel(BackendChannel& channel, const QString& postId);

    Backend& backend;
    HTTPConnector httpConnector;
};

} // namespace Mattermost
