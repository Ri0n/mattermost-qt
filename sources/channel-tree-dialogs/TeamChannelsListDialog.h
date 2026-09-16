/**
 * @file TeamChannelsListDialog.h
 * @brief Lazily loaded public-channel directory for one Mattermost team.
 */

#pragma once

#include <list>

#include <QJsonArray>
#include <QSet>
#include <QVector>

#include "FilterListDialog.h"
#include "backend/types/BackendChannel.h"

class QLabel;
class QTimer;

namespace Mattermost {

class BackendTeam;
class LongListWidget;

class TeamChannelsListDialog final : public FilterListDialog
{
public:
    TeamChannelsListDialog(Backend& backend,
                           const FilterListDialogConfig& cfg,
                           BackendTeam& team,
                           QWidget* parent);
    ~TeamChannelsListDialog() override;

    void addContextMenuActions(QMenu& menu, const QVariant& selectedItemData) override;
    void setItemCountLabel(uint32_t count) override;

private:
    struct PendingRange {
        int first = -1;
        int last = -1;
        QSet<int> pages;
    };

    void setupVirtualList(const FilterListDialogConfig& cfg);
    QWidget* createHeaderRow();
    QWidget* createChannelRow(int index);
    BackendChannel* channelAt(int index) const;
    void showChannelContextMenu(BackendChannel* channel, const QPoint& globalPos);

    void requestChannelRange(int first, int last);
    void loadPage(int page);
    void completePage(int page);
    void finishAllRequests();
    void reapplyLoadedPages();
    void updatePagedCountLabel(bool hasMore);

    void filterEdited(const QString& text);
    void performSearch();
    void enterSearchResults(QJsonArray results, int generation);
    void restorePagedMode();

    Backend& backend;
    BackendTeam& team;
    LongListWidget* channelList = nullptr;
    QTimer* searchTimer = nullptr;

    std::list<BackendChannel> pageStorage;
    QVector<BackendChannel*> pageChannels;
    QSet<int> loadedPages;
    QSet<int> inFlightPages;
    QVector<PendingRange> pendingRanges;
    int pagedItemCount = 0;
    int concretePagedCount = 0;
    bool pagedEndKnown = false;

    std::list<BackendChannel> searchStorage;
    QVector<BackendChannel*> searchChannels;
    QString searchTerm;
    int searchGeneration = 0;
    bool searchMode = false;
};

} // namespace Mattermost
