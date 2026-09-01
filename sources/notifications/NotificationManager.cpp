/**
 * @file NotificationManager.cpp
 * @brief Desktop notifications with per-notification activation targets.
 */

#include "NotificationManager.h"

#include <QApplication>
#include <QStringList>
#include <QSystemTrayIcon>
#include <QTimer>

#ifdef MATTERMOST_QT_FREEDESKTOP_NOTIFICATIONS
#include <QVariantMap>
#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusInterface>
#include <QtDBus/QDBusMessage>
#include <QtDBus/QDBusReply>
#endif

namespace Mattermost {

NotificationManager::NotificationManager(QSystemTrayIcon& trayIcon, QObject* parent)
    : QObject(parent)
#ifdef MATTERMOST_QT_FREEDESKTOP_NOTIFICATIONS
    , freedesktopActionsSupported(false)
#endif
    , trayIcon(trayIcon)
{
    connect(&trayIcon, &QSystemTrayIcon::messageClicked,
            this, &NotificationManager::onFallbackMessageClicked);

#ifdef MATTERMOST_QT_FREEDESKTOP_NOTIFICATIONS
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        return;
    }

    QDBusInterface notifications(QStringLiteral("org.freedesktop.Notifications"),
                                 QStringLiteral("/org/freedesktop/Notifications"),
                                 QStringLiteral("org.freedesktop.Notifications"), bus);
    if (!notifications.isValid()) {
        return;
    }

    const QDBusReply<QStringList> capabilities = notifications.call(QStringLiteral("GetCapabilities"));
    freedesktopActionsSupported = capabilities.isValid()
        && capabilities.value().contains(QStringLiteral("actions"));
    if (!freedesktopActionsSupported) {
        return;
    }

    bus.connect(QStringLiteral("org.freedesktop.Notifications"),
                QStringLiteral("/org/freedesktop/Notifications"),
                QStringLiteral("org.freedesktop.Notifications"),
                QStringLiteral("ActionInvoked"),
                this, SLOT(onActionInvoked(uint,QString)));
    bus.connect(QStringLiteral("org.freedesktop.Notifications"),
                QStringLiteral("/org/freedesktop/Notifications"),
                QStringLiteral("org.freedesktop.Notifications"),
                QStringLiteral("NotificationClosed"),
                this, SLOT(onNotificationClosed(uint,uint)));
#endif
}

void NotificationManager::show(const QString& title, const QString& body,
                               const NotificationTarget& target)
{
#ifdef MATTERMOST_QT_FREEDESKTOP_NOTIFICATIONS
    if (freedesktopActionsSupported && showFreedesktop(title, body, target)) {
        return;
    }
#endif

    fallbackTarget = target;
    trayIcon.showMessage(title, body, QSystemTrayIcon::Information);
}

void NotificationManager::onFallbackMessageClicked()
{
    if (!fallbackTarget.isValid()) {
        return;
    }

    const NotificationTarget target = fallbackTarget;
    fallbackTarget = NotificationTarget {};
    emit activated(target);
}

#ifdef MATTERMOST_QT_FREEDESKTOP_NOTIFICATIONS
bool NotificationManager::showFreedesktop(const QString& title, const QString& body,
                                           const NotificationTarget& target)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        return false;
    }

    QDBusInterface notifications(QStringLiteral("org.freedesktop.Notifications"),
                                 QStringLiteral("/org/freedesktop/Notifications"),
                                 QStringLiteral("org.freedesktop.Notifications"), bus);
    if (!notifications.isValid()) {
        return false;
    }

    const QStringList actions {
        QStringLiteral("default"),
        tr("Open"),
    };
    const QVariantMap hints {
        {QStringLiteral("desktop-entry"), QStringLiteral("mattermost-qt")},
    };
    const QList<QVariant> arguments {
        qApp->applicationName(),
        QVariant::fromValue(uint(0)),
        QStringLiteral("mattermost-qt"),
        title,
        body,
        actions,
        hints,
        -1,
    };

    const QDBusMessage message = notifications.callWithArgumentList(
        QDBus::Block, QStringLiteral("Notify"), arguments);
    const QDBusReply<uint> reply(message);
    if (!reply.isValid()) {
        return false;
    }

    freedesktopTargets.insert(reply.value(), target);
    return true;
}

void NotificationManager::onActionInvoked(uint notificationId, const QString& actionKey)
{
    if (actionKey != QStringLiteral("default")) {
        return;
    }

    const auto it = freedesktopTargets.constFind(notificationId);
    if (it == freedesktopTargets.cend()) {
        return;
    }

    const NotificationTarget target = it.value();
    freedesktopTargets.remove(notificationId);
    emit activated(target);
}

void NotificationManager::onNotificationClosed(uint notificationId, uint reason)
{
    Q_UNUSED(reason);

    // Some notification servers close a notification as part of invoking its
    // default action. The specification does not guarantee whether
    // NotificationClosed or ActionInvoked is delivered first, so retain the
    // target briefly to let a following ActionInvoked resolve the same ID.
    QTimer::singleShot(5000, this, [this, notificationId] {
        freedesktopTargets.remove(notificationId);
    });
}
#endif

} // namespace Mattermost
