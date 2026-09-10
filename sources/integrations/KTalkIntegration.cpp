/**
 * @file KTalkIntegration.cpp
 * @brief Native adapter for a server-provided KTalk Mattermost plugin.
 *
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#include "KTalkIntegration.h"

#include <utility>

#include <QAction>
#include <QCheckBox>
#include <QDesktopServices>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMainWindow>
#include <QMessageBox>
#include <QNetworkReply>
#include <QPixmap>
#include <QPushButton>
#include <QStyle>
#include <QToolBar>
#include <QUrl>

#include "backend/Backend.h"
#include "backend/NetworkRequest.h"
#include "backend/ServerUiService.h"
#include "backend/WebappPluginService.h"
#include "backend/types/BackendChannel.h"

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
        return QString::fromUtf8(QJsonDocument(error.toObject()).toJson(QJsonDocument::Compact));
    }
    return object.value(QStringLiteral("message")).toString();
}

QUrl meetingUrl(const QJsonObject& data)
{
    const QUrl url(data.value(QStringLiteral("meeting_url")).toString());
    if (!url.isValid()
        || (url.scheme() != QLatin1String("https") && url.scheme() != QLatin1String("http"))) {
        return {};
    }
    return url;
}

} // namespace

KTalkIntegration& KTalkIntegration::install(QMainWindow& window, Backend& backend)
{
    auto* integration = window.findChild<KTalkIntegration*>(
        QString(), Qt::FindDirectChildrenOnly);
    if (!integration) {
        integration = new KTalkIntegration(window, backend);
    }
    return *integration;
}

KTalkIntegration::KTalkIntegration(QMainWindow& window, Backend& backend)
    : QObject(&window)
    , window_(window)
    , backend_(backend)
{
    toolbar_ = new QToolBar(tr("Integrations"), &window_);
    toolbar_->setObjectName(QStringLiteral("integrationAppBar"));
    toolbar_->setMovable(false);
    toolbar_->setFloatable(false);
    toolbar_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    toolbar_->setIconSize(QSize(24, 24));

    QIcon icon = QIcon::fromTheme(QStringLiteral("camera-video"));
    if (icon.isNull()) {
        icon = window_.style()->standardIcon(QStyle::SP_MediaPlay);
    }
    action_ = toolbar_->addAction(icon, tr("Start KTalk Meeting"));
    action_->setToolTip(tr("Start KTalk Meeting"));
    action_->setVisible(false);
    toolbar_->hide();
    window_.addToolBar(Qt::RightToolBarArea, toolbar_);

    connect(action_, &QAction::triggered, this, &KTalkIntegration::startMeeting);

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
    }

    const bool available = !pluginId_.isEmpty();
    action_->setVisible(available);
    toolbar_->setVisible(available);

    if (available) {
        qCDebug(lcKTalk) << "Native KTalk integration enabled by server discovery";
        requestAppBarIcon();
    }
}

void KTalkIntegration::requestAppBarIcon()
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
                qCDebug(lcKTalk) << "KTalk App Bar icon request failed";
                return;
            }

            QPixmap pixmap;
            if (pixmap.loadFromData(data)) {
                action_->setIcon(QIcon(pixmap));
            }
        }));
}

void KTalkIntegration::startMeeting()
{
    BackendChannel* channel = backend_.getCurrentChannel();
    if (!channel || channel->id.isEmpty()) {
        showError(tr("Open a channel or conversation before starting a KTalk meeting."));
        return;
    }
    if (pluginId_.isEmpty()) {
        showError(tr("KTalk integration is not available on this server."));
        return;
    }

    QMessageBox dialog(QMessageBox::Question,
                       tr("Start KTalk Meeting"),
                       tr("Are you sure you want to start a call in KTalk?"),
                       QMessageBox::Cancel,
                       &window_);
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
    submitStartMeeting(channel->id, callWithSound->isChecked());
}

void KTalkIntegration::submitStartMeeting(const QString& channelId, bool callEveryone)
{
    if (pluginId_.isEmpty()) {
        return;
    }

    QJsonObject payload {
        {QStringLiteral("channel_id"), channelId},
        {QStringLiteral("topic"), QString()},
        {QStringLiteral("root_id"), QString()},
        {QStringLiteral("call_everyone"), callEveryone},
    };

    const QString activePluginId = pluginId_;
    NetworkRequest request(
        QString(), pluginPath(activePluginId, QLatin1String(StartMeetingSuffix)));
    httpConnector_.post(request, payload, HttpResponseCallback(
        [this, activePluginId](QVariant status,
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
                showError(error.isEmpty()
                              ? tr("Error occurred while starting the KTalk meeting.")
                              : error);
                return;
            }

            const QString error = responseError(data);
            if (!error.isEmpty()) {
                showError(error);
                return;
            }

            qCDebug(lcKTalk) << "KTalk meeting creation accepted";
        }));
}

void KTalkIntegration::handleCustomWebSocketEvent(const QString& eventName,
                                                   const QJsonObject& data)
{
    if (pluginId_.isEmpty() || eventName != meetingStartedEvent(pluginId_)) {
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

void KTalkIntegration::showError(const QString& message)
{
    QMessageBox::warning(&window_, tr("KTalk"), message);
}

} // namespace Mattermost
