#include "LocalPostDeleteTracker.h"

#include <QSet>
#include <QStringList>
#include <QUrl>

namespace Mattermost {
namespace LocalPostDeleteTracker {
namespace {

QSet<QString> pendingPostIds;

QString postIdFromDeleteUrl(const QUrl& url)
{
    const QStringList parts = url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.size() < 2 || parts.at(parts.size() - 2) != QStringLiteral("posts")) {
        return {};
    }
    return parts.constLast();
}

} // namespace

void noteRequest(const QUrl& url)
{
    const QString postId = postIdFromDeleteUrl(url);
    if (!postId.isEmpty()) {
        pendingPostIds.insert(postId);
    }
}

void clearFailedRequest(const QUrl& url)
{
    const QString postId = postIdFromDeleteUrl(url);
    if (!postId.isEmpty()) {
        pendingPostIds.remove(postId);
    }
}

bool take(const QString& postId)
{
    return !postId.isEmpty() && pendingPostIds.remove(postId) > 0;
}

void clear()
{
    pendingPostIds.clear();
}

} // namespace LocalPostDeleteTracker
} // namespace Mattermost
