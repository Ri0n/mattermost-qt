#pragma once

#include <QString>

class QUrl;

namespace Mattermost {
namespace LocalPostDeleteTracker {

/** Record a locally initiated DELETE /posts/{post_id} request. */
void noteRequest(const QUrl& url);

/** Drop a recorded request when the HTTP DELETE fails before a websocket event. */
void clearFailedRequest(const QUrl& url);

/** Consume and return whether this post deletion originated in this client. */
bool take(const QString& postId);

/** Clear transient state when the HTTP connector is reset. */
void clear();

} // namespace LocalPostDeleteTracker
} // namespace Mattermost
