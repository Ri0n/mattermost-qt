/**
 * @file KTalkIntegration.cpp
 * @brief Native adapter for a server-provided KTalk Mattermost plugin.
 *
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#include "KTalkIntegration.h"

#include <QApplication>
#include <QCheckBox>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QNetworkReply>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QStyle>
#include <QUrl>
#include <QWidget>

#include "backend/Backend.h"
#include "backend/NetworkRequest.h"
#include "backend/ServerUiService.h"
#include "backend/WebappPluginService.h"

namespace Mattermost {
namespace {

constexpr char KTalkMarker[] = "ktalk";
constexpr char MeetingEventSuffix[] = "_ktalk_meeting_started";
constexpr char StartMeetingSuffix[] = "api/v1/meetings";
constexpr char AppBarIconSuffix[] = "public/app-bar-icon.png";

Q_LOGGING_CATEGORY(lcKTalk, "mattermost.ktalk")

bool isKTalkPlugin(const WebappPluginManifest& manifest)
{
    const auto containsMarker = [](const QString& value) {
        return value.contains(QLatin1String(KTalkMarker), Qt::CaseInsensitive);
    };

    return containsMarker(manifest.id)
        || containsMarker(manifest.name)
        || containsMarker(manifest.description)
        || containsMarker(manifest.bundlePath);
}

QString pluginPath(const QString& pluginId, const QString& suffix)
{
    if (pluginId.isEmpty()) {
        return {};
    }
    return QStringLiteral("plugins/") + pluginId + QLatin1Char('/') + suffix;
}

QString meetingStartedEvent(const QString& pluginId)
{
    return pluginId.isEmpty()
        ? QString()
        : QStringLiteral("custom_") + pluginId
            + QLatin1String(MeetingEventSuffix);
}

QIcon defaultVideoIcon()
{
    QIcon icon = QIcon::fromTheme(QStringLiteral("camera-video"));
    if (icon.isNull() && QApplication::style()) {
        icon = QApplication::style()->standardIcon(QStyle::SP_MediaPlay);
    }
    return icon;
}

QString responseError(const QByteArray& data)
{
    const QJsonDocument document = QJsonDocument::fromJson(data);
    if (!document.isObject()) {
        return QString::fromUtf8(data).trimmed();
    }

    const QJsonObject object = document.object();
    const QJsonValue error = object.value(QStringLiteral("error"));
    if (error.isString()) {
        return error.toString();
    }
    if (!error.isNull() && !error.isUndefined()) {
        return QString::fromUtf8(
            QJsonDocument(error.toObject()).toJson(QJsonDocument::Compact));
    }
    return object.value(QStringLiteral("message")).toString();
}

QUrl meetingUrl(const QJsonObject& data)
{
    const QUrl url(data.value(QStringLiteral("meeting_url")).toString());
    if (!url.isValid()
        || (url.scheme() != QLatin1String("https")
            && url.scheme() != QLatin1String("http"))) {
        return {};
    }
    return url;
}

} // namespace

KTalkIntegration& KTalkIntegration::instance(Backend& backend)
{
    auto* integration = backend.findChild<KTalkIntegration*>(
        QString(), Qt::FindDirectChildrenOnly);
    if (!integration) {
        integration = new KTalkIntegration(backend);
    }
    return *integration;
}

KTalkIntegration::KTalkIntegration(Backend& backend)
    : QObject(&backend)
    , backend_(backend)
    , icon_(defaultVideoIcon())
{
    auto& plugins = WebappPluginService::instance(backend_);
    connect(&plugins, &WebappPluginService::pluginsChanged,
            this, &KTalkIntegration::refreshAvailability);

    auto& serverUi = ServerUiService::instance(backend_);
    connect(&serverUi, &ServerUiService::customWebSocketEventReceived,
            this,
            [this](const QString& eventName,
                   const QJsonObject& data,
                   const QJsonObject&) {
        handleCustomWebSocketEvent(eventName, data);
    });

    refreshAvailability();
    plugins.ensureLoaded();
}

void KTalkIntegration::refreshAvailability()
{
    const bool wasAvailable = isAvailable();
    const auto& plugins = WebappPluginService::instance(backend_).plugins();
    QString detectedPluginId;
    for (const WebappPluginManifest& manifest : plugins) {
        if (isKTalkPlugin(manifest)) {
            detectedPluginId = manifest.id;
            break;
        }
    }

    if (pluginId_ != detectedPluginId) {
        httpConnector_.reset();
        pluginId_ = detectedPluginId;
        iconRequested_ = false;
        icon_ = defaultVideoIcon();
        emit iconChanged();
    }

    const bool available = isAvailable();
    if (wasAvailable != available) {
        emit availabilityChanged(available);
    }

    if (available) {
        qCDebug(lcKTalk) << "Native KTalk integration enabled by server discovery";
        requestIcon();
    }
}

void KTalkIntegration::requestIcon()
{
    if (iconRequested_ || pluginId_.isEmpty()) {
        return;
    }
    iconRequested_ = true;

    const QString requestedPluginId = pluginId_;
    NetworkRequest request(
        QString(), pluginPath(requestedPluginId, QLatin1String(AppBarIconSuffix)), true);
    request.setPriority(QNetworkRequest::LowPriority);
    httpConnector_.get(request, HttpResponseCallback(
        [this, requestedPluginId](QVariant status, QByteArray data) {
            if (requestedPluginId != pluginId_) {
                return;
            }
            if (status.toInt() != QNetworkReply::NoError || data.isEmpty()) {
                qCDebug(lcKTalk) << "KTalk icon request failed";
                return;
            }

            QPixmap pixmap;
            if (!pixmap.loadFromData(data)) {
                return;
            }

            icon_ = QIcon(pixmap);
            emit iconChanged();
        }));
}

void KTalkIntegration::startMeeting(QWidget* parent,
                                    const QString& channelId,
                                    const QString& rootId)
{
    if (channelId.isEmpty()) {
        showError(parent,
                  tr("Open a channel or conversation before starting a KTalk meeting."));
        return;
    }
    if (!isAvailable()) {
        showError(parent, tr("KTalk integration is not available on this server."));
        return;
    }

    QMessageBox dialog(QMessageBox::Question,
                       tr("Start KTalk Meeting"),
                       rootId.isEmpty()
                           ? tr("Are you sure you want to start a call in this conversation?")
                           : tr("Are you sure you want to start a call in this thread?"),
                       QMessageBox::Cancel,
                       parent);
    auto* callWithSound = new QCheckBox(tr("Call with sound"), &dialog);
    dialog.setCheckBox(callWithSound);
    QPushButton* callButton = dialog.addButton(tr("Call"), QMessageBox::AcceptRole);
    dialog.setDefaultButton(callButton);

    dialog.exec();
    if (dialog.clickedButton() != callButton) {
        return;
    }

    // The web UI labels this option "Call with sound" while the plugin API
    // names the field call_everyone. Preserve that wire behaviour.
    submitStartMeeting(parent,
                       channelId,
                       rootId,
                       callWithSound->isChecked());
}

void KTalkIntegration::submitStartMeeting(QWidget* parent,
                                          const QString& channelId,
                                          const QString& rootId,
                                          bool callEveryone)
{
    if (!isAvailable()) {
        return;
    }

    QJsonObject payload {
        {QStringLiteral("channel_id"), channelId},
        {QStringLiteral("topic"), QString()},
        {QStringLiteral("root_id"), rootId},
        {QStringLiteral("call_everyone"), callEveryone},
    };

    const QString activePluginId = pluginId_;
    QPointer<QWidget> parentGuard(parent);
    NetworkRequest request(
        QString(), pluginPath(activePluginId, QLatin1String(StartMeetingSuffix)));
    httpConnector_.post(request, payload, HttpResponseCallback(
        [this, activePluginId, parentGuard](QVariant status,
                                            QByteArray data,
                                            const QNetworkReply& reply) {
            if (activePluginId != pluginId_) {
                return;
            }
            if (status.toInt() != QNetworkReply::NoError) {
                QString error = responseError(data);
                if (error.isEmpty()) {
                    error = reply.errorString();
                }
                showError(parentGuard.data(),
                          error.isEmpty()
                              ? tr("Error occurred while starting the KTalk meeting.")
                              : error);
                return;
            }

            const QString error = responseError(data);
            if (!error.isEmpty()) {
                showError(parentGuard.data(), error);
                return;
            }

            qCDebug(lcKTalk) << "KTalk meeting creation accepted";
        }));
}

void KTalkIntegration::handleCustomWebSocketEvent(const QString& eventName,
                                                   const QJsonObject& data)
{
    if (!isAvailable() || eventName != meetingStartedEvent(pluginId_)) {
        return;
    }

    const QUrl url = meetingUrl(data);
    if (url.isEmpty()) {
        qCWarning(lcKTalk) << "KTalk meeting-start event has no valid meeting_url";
        return;
    }

    qCDebug(lcKTalk) << "Opening KTalk meeting from server event";
    QDesktopServices::openUrl(url);
}

void KTalkIntegration::showError(QWidget* parent, const QString& message)
{
    QMessageBox::warning(parent, tr("KTalk"), message);
}

} // namespace Mattermost
