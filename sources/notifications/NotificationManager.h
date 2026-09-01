/**
 * @file NotificationManager.h
 * @brief Desktop notifications with per-notification activation targets.
 */

#pragma once

#include <QObject>
#include <QString>

#ifdef MATTERMOST_QT_FREEDESKTOP_NOTIFICATIONS
#include <QMap>
#endif

class QSystemTrayIcon;

namespace Mattermost {

struct NotificationTarget {
    QString channelId;
    QString postId;
    QString rootId;

    bool isValid() const
    {
        return !channelId.isEmpty() && !postId.isEmpty();
    }
};

class NotificationManager : public QObject {
    Q_OBJECT
public:
    explicit NotificationManager(QSystemTrayIcon& trayIcon, QObject* parent = nullptr);

    void show(const QString& title, const QString& body, const NotificationTarget& target);

signals:
    void activated(const NotificationTarget& target);

private slots:
    void onFallbackMessageClicked();

#ifdef MATTERMOST_QT_FREEDESKTOP_NOTIFICATIONS
    void onActionInvoked(uint notificationId, const QString& actionKey);
    void onNotificationClosed(uint notificationId, uint reason);
#endif

private:
#ifdef MATTERMOST_QT_FREEDESKTOP_NOTIFICATIONS
    bool showFreedesktop(const QString& title, const QString& body,
                         const NotificationTarget& target);
    bool freedesktopActionsSupported;
    QMap<uint, NotificationTarget> freedesktopTargets;
#endif

    QSystemTrayIcon& trayIcon;
    NotificationTarget fallbackTarget;
};

} // namespace Mattermost
