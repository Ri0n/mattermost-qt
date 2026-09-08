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
#include "backend/Storage.h"
#include "backend/types/BackendDirectChannelsTeam.h"
#include "channel-tree/AttentionList.h"
#include "channel-tree/ChannelQuickList.h"
#include "channel-tree/ChannelTree.h"
#include "post-collection/PostCollectionView.h"

namespace Mattermost {

void MainWindow::installRealtimeUiSync()
{
    static constexpr char InstalledProperty[] = "_mmqt_realtime_ui_sync_installed";
    if (property(InstalledProperty).toBool()) {
        return;
    }
    setProperty(InstalledProperty, true);

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
