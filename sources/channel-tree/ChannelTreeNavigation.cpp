#include "ChannelTree.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QPointer>
#include <QTreeWidgetItem>

#include "backend/Backend.h"
#include "backend/HTTPConnector.h"
#include "backend/HttpResponseCallback.h"
#include "backend/NetworkRequest.h"
#include "backend/QByteArrayCreator.h"
#include "backend/SidebarService.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendTeam.h"
#include "channel-tree/ChannelItem.h"
#include "channel-tree/team-item/TeamItem.h"

namespace Mattermost {
namespace {

bool containsChannel(const SidebarTeamState& state, const QString& channelId)
{
    for (auto it = state.categories.cbegin(); it != state.categories.cend(); ++it) {
        if (it->channelIds.contains(channelId)) {
            return true;
        }
    }
    return false;
}

} // namespace

void ChannelTree::openStoredChannel(QString channelID)
{
    auto existing = channelToItemMap.constFind(channelID);
    if (existing != channelToItemMap.cend() && !existing.value().isEmpty()) {
        openChannel(std::move(channelID));
        return;
    }

    if (!backendForSidebar) {
        emit storedChannelOpenFinished(channelID, false);
        return;
    }

    BackendChannel* channel = backendForSidebar->getStorage().getChannelById(channelID);
    if (!channel) {
        qDebug() << "openStoredChannel" << channelID << ": channel not found in storage";
        emit storedChannelOpenFinished(channelID, false);
        return;
    }

    // A direct_added event can populate Storage before the server category
    // snapshot catches up. Reconcile that conversation with every loaded
    // Direct Messages category first; in the normal realtime path this also
    // materializes the row immediately.
    if (channel->type == BackendChannel::directChannel
        || channel->type == BackendChannel::groupChannel) {
        admitStoredConversation(*channel);
        existing = channelToItemMap.constFind(channelID);
        if (existing != channelToItemMap.cend() && !existing.value().isEmpty()) {
            openChannel(std::move(channelID));
            return;
        }
    }

    auto& sidebar = SidebarService::instance(*backendForSidebar);

    // Prefer the Direct Messages category when a channel occurs in more than
    // one server category. A materialized current DM is retained by the sidebar
    // policy on subsequent refreshes even when it is outside the normal limit.
    struct Candidate {
        TeamItem* teamItem = nullptr;
        QTreeWidgetItem* categoryItem = nullptr;
        bool directMessages = false;
    };
    Candidate fallback;

    for (auto teamIt = teamToItemMap.cbegin(); teamIt != teamToItemMap.cend(); ++teamIt) {
        TeamItem* teamItem = teamIt.value();
        const SidebarTeamState* state = sidebar.teamState(teamIt.key());
        if (!teamItem || !state) {
            continue;
        }

        for (const QString& categoryId : state->order) {
            const SidebarCategory* category = state->category(categoryId);
            if (!category || !category->channelIds.contains(channelID)) {
                continue;
            }

            QTreeWidgetItem* categoryItem = nullptr;
            for (int i = 0; i < teamItem->childCount(); ++i) {
                QTreeWidgetItem* child = teamItem->child(i);
                if (child && child->data(0, ItemKindRole).toInt() == CategoryItemKind
                    && child->data(0, ItemIdRole).toString() == categoryId) {
                    categoryItem = child;
                    break;
                }
            }
            if (!categoryItem) {
                continue;
            }

            const bool isDirectMessages = category->type == QStringLiteral("direct_messages");
            if (isDirectMessages) {
                fallback = Candidate {teamItem, categoryItem, true};
                break;
            }
            if (!fallback.categoryItem) {
                fallback = Candidate {teamItem, categoryItem, false};
            }
        }

        if (fallback.directMessages) {
            break;
        }
    }

    if (!fallback.teamItem || !fallback.categoryItem) {
        // A permalink can resolve a public channel that is visible to the user
        // but not yet joined. First refresh categories in case only our local
        // snapshot is stale; if the channel is still absent, join it and retry
        // only after the server category snapshot confirms membership.
        if (channel->type != BackendChannel::publicChannel || !channel->team) {
            qDebug() << "openStoredChannel" << channelID
                     << ": no sidebar category contains channel";
            emit storedChannelOpenFinished(channelID, false);
            return;
        }
        if (pendingChannelJoins.contains(channelID)) {
            return;
        }

        pendingChannelJoins.insert(channelID);
        const QString teamId = channel->team->id;
        QPointer<ChannelTree> guard(this);
        sidebar.retrieveCategories(*channel->team,
            [guard, channelID, teamId](const SidebarTeamState& refreshedState) {
                if (!guard || !guard->backendForSidebar) {
                    return;
                }

                if (containsChannel(refreshedState, channelID)) {
                    guard->pendingChannelJoins.remove(channelID);
                    guard->openStoredChannel(channelID);
                    QTreeWidgetItem* current = guard->currentItem();
                    const bool opened = current
                        && current->data(0, ItemKindRole).toInt() == ChannelItemKind
                        && current->data(0, ItemIdRole).toString() == channelID;
                    emit guard->storedChannelOpenFinished(channelID, opened);
                    return;
                }

                BackendChannel* currentChannel =
                    guard->backendForSidebar->getStorage().getChannelById(channelID);
                if (!currentChannel || currentChannel->type != BackendChannel::publicChannel) {
                    guard->pendingChannelJoins.remove(channelID);
                    qWarning() << "Cannot auto-join non-public channel" << channelID;
                    emit guard->storedChannelOpenFinished(channelID, false);
                    return;
                }

                auto* connector = new HTTPConnector;
                connector->setParent(guard);
                QObject::connect(connector, &HTTPConnector::onNetworkError,
                                 guard->backendForSidebar, &Backend::onNetworkError);
                QObject::connect(connector, &HTTPConnector::onHttpError,
                                 guard->backendForSidebar, &Backend::onHttpError);

                NetworkRequest request(QStringLiteral("channels/") + channelID
                                       + QStringLiteral("/members"));
                const QJsonObject payload {
                    {QStringLiteral("user_id"), guard->backendForSidebar->getLoginUser().id},
                };
                QPointer<HTTPConnector> connectorGuard(connector);
                connector->post(request, QByteArrayCreator(payload), HttpResponseCallback(
                    [guard, connectorGuard, channelID, teamId](
                        QVariant status, const QJsonDocument&) {
                        if (connectorGuard) {
                            connectorGuard->deleteLater();
                        }
                        if (!guard || !guard->backendForSidebar) {
                            return;
                        }

                        if (status.toInt() != QNetworkReply::NoError) {
                            guard->pendingChannelJoins.remove(channelID);
                            qWarning() << "Failed to auto-join public channel" << channelID
                                       << "network status" << status.toInt();
                            emit guard->storedChannelOpenFinished(channelID, false);
                            return;
                        }

                        BackendTeam* team =
                            guard->backendForSidebar->getStorage().getTeamById(teamId);
                        if (!team) {
                            guard->pendingChannelJoins.remove(channelID);
                            qWarning() << "Joined channel" << channelID
                                       << "but team disappeared" << teamId;
                            emit guard->storedChannelOpenFinished(channelID, false);
                            return;
                        }

                        SidebarService::instance(*guard->backendForSidebar).retrieveCategories(
                            *team,
                            [guard, channelID](const SidebarTeamState& joinedState) {
                                if (!guard) {
                                    return;
                                }
                                guard->pendingChannelJoins.remove(channelID);
                                if (!containsChannel(joinedState, channelID)) {
                                    qWarning() << "Joined channel" << channelID
                                               << "but it is still absent from sidebar categories";
                                    emit guard->storedChannelOpenFinished(channelID, false);
                                    return;
                                }

                                guard->openStoredChannel(channelID);
                                QTreeWidgetItem* current = guard->currentItem();
                                const bool opened = current
                                    && current->data(0, ItemKindRole).toInt() == ChannelItemKind
                                    && current->data(0, ItemIdRole).toString() == channelID;
                                emit guard->storedChannelOpenFinished(channelID, opened);
                            });
                    }));
            });
        return;
    }

    ChannelItem* item = createChannelItem(*backendForSidebar,
                                          *fallback.teamItem,
                                          *fallback.categoryItem,
                                          *channel);
    if (!item) {
        emit storedChannelOpenFinished(channelID, false);
        return;
    }

    fallback.categoryItem->setExpanded(true);
    if (currentItem() == item) {
        activateChannelItem(item);
    } else {
        setCurrentItem(item);
    }
}

} // namespace Mattermost
