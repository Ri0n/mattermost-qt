#pragma once

#include <QObject>
#include <QTimer>

namespace Mattermost {

class Backend;

/**
 * Keeps the active channel usable when WebSocket transport is unavailable.
 *
 * WebSocket remains the preferred realtime transport and continues its normal
 * reconnect/backoff loop. While it is disconnected, this service polls the
 * active channel over REST and feeds newly discovered posts through the same
 * live channel/backend signals used by realtime delivery.
 */
class RealtimeFallbackService final : public QObject
{
public:
    static RealtimeFallbackService& instance(Backend& backend);

private:
    explicit RealtimeFallbackService(Backend& backend);

    void startPolling();
    void stopPolling();
    void pollNow();

    Backend& backend;
    QTimer pollTimer;
    bool polling = false;
    bool requestInFlight = false;
};

} // namespace Mattermost
