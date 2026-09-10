/**
 * @file MainWindowRealtime.cpp
 * @brief Cross-view synchronization for realtime conversation state.
 *
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#include "mainwindow.h"

#include "ui_mainwindow.h"

#include "backend/Backend.h"
#include "backend/ServerUiService.h"
#include "backend/Storage.h"
#include "backend/WebappPluginService.h"
#include "backend/types/BackendDirectChannelsTeam.h"
#include "channel-tree/AttentionList.h"
#include "channel-tree/ChannelQuickList.h"
#include "channel-tree/ChannelTree.h"
#include "integrations/KTalkIntegration.h"
#include "navigation/AppNavigationService.h"
#include "notifications/NotificationManager.h"
#include "post-collection/PostCollectionView.h"
#include "server-dialog/ServerDialog.h"

namespace Mattermost {

void MainWindow::installRealtimeUiSync()
{
    static constexpr char InstalledProperty[] = "_mmqt_realtime_ui_sync_installed";
    if (property(InstalledProperty).toBool()) {
        return;
    }
    setProperty(InstalledProperty, true);

    // Active webapp plugins are the server-owned discovery surface for native
    // integrations too. Keep the manifest snapshot session-local; adapters can
    // subscribe to pluginsChanged() and derive installation-specific endpoints
    // from the advertised manifest without executing its JavaScript bundle.
    auto& webappPlugins = WebappPluginService::instance(backend);
    webappPlugins.clear();
    webappPlugins.refresh();
    KTalkIntegration::install(*this, backend);

    // Notification post targets use the same semantic navigation service as
    // permalinks, Following and Attention. A reply can therefore load its root
    // and exact thread position before the concrete target widget is presented.
    connect(notificationManager.get(), &NotificationManager::activated,
            this, [this](const NotificationTarget& target) {
        if (!target.isValid()) {
            return;
        }

        if (isMinimized()) {
            showNormal();
        } else {
            show();
        }
        raise();
        activateWindow();

        AppNavigationService::instance(backend).openPost(target.postId);
    });

    // Server-originated ephemeral posts are transient by definition: surface
    // them through the existing desktop notification path rather than adding
    // them to PostRepository or a channel timeline. Interactive dialogs remain
    // native Qt dialogs and never run a nested event loop.
    auto& serverUi = ServerUiService::instance(backend);
    connect(&serverUi, &ServerUiService::ephemeralMessageReceived,
            this, [this](const QString& message) {
        notificationManager->show(tr("Mattermost"), message, NotificationTarget {});
    });
    connect(&serverUi, &ServerUiService::interactiveDialogRequested,
            this, [this](const QJsonObject& dialog,
                         const QString& url,
                         const QString& channelId,
                         const QString& teamId) {
        auto* serverDialog = new ServerDialog(
            backend, dialog, url, channelId, teamId, this);
        serverDialog->show();
    });

    // Alternate sidebar views can legitimately know about a direct/group
    // conversation before the server-backed Channels tree has materialized its
    // row. Route them through the Storage-aware navigation path rather than
    // requiring channelToItemMap to be populated already.
    disconnect(recentChannels, &ChannelQuickList::channelSelected,
               ui->channelList, &ChannelTree::openChannel);
    connect(recentChannels, &ChannelQuickList::channelSelected,
            ui->channelList, &ChannelTree::openStoredChannel);

    disconnect(attentionList, &AttentionList::channelSelected,
               ui->channelList, &ChannelTree::openChannel);
    connect(attentionList, &AttentionList::channelSelected,
            ui->channelList, &ChannelTree::openStoredChannel);

    auto connectConversationSource = [this](BackendDirectChannelsTeam& conversations) {
        connect(&conversations, &BackendDirectChannelsTeam::onNewChannel,
                this, [this](BackendChannel& channel) {
            // direct_added/channel fetch updates Storage first. Admit that
            // authoritative object into the already loaded Direct Messages
            // categories immediately; no category HTTP round-trip is needed.
            ui->channelList->admitStoredConversation(channel);
            refreshSidebarViews();
        });
    };
    connectConversationSource(backend.getStorage().directChannels);
    connectConversationSource(backend.getStorage().groupChannels);

    // Mattermost synchronizes flagged_post through preference websocket
    // events. Feed them into the collection only when it has been opened; its
    // initial activation already loads the authoritative current snapshot.
    connect(&backend, &Backend::onFlaggedPostChanged,
            this, [this](const QString& postId, bool flagged) {
        if (savedMessagesPage) {
            savedMessagesPage->syncFlaggedPost(postId, flagged);
        }
    });
}

} // namespace Mattermost
