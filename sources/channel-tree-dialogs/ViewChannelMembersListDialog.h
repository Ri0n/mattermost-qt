/**
 * @file ViewChannelMembersListDialog.h
 * @brief 'View Channel Members' context menu item dialog
 * @author Lyubomir Filipov
 * @date Apr 17, 2023
 *
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Mattermost-QT is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Mattermost-QT. if not, see https://www.gnu.org/licenses/.
 */

#pragma once

#include <QSet>
#include <QStringList>
#include <QVector>

#include "UserListDialog.h"

class QTimer;
class QWidget;

namespace Mattermost {

class BackendChannel;
class BackendUser;
class LongListWidget;

class ViewChannelMembersListDialog: public UserListDialog {
public:
    ViewChannelMembersListDialog(Backend& backend,
                                 BackendChannel& channel,
                                 QWidget* parent);
    ~ViewChannelMembersListDialog() override;

private:
    struct PendingRange {
        int first = -1;
        int last = -1;
    };

    void addContextMenuActions(QMenu& menu,
                               const QVariant& selectedItemData) override;

    void setupVirtualList();
    void reloadMemberCount();
    void resetPagedMembers();
    void requestMemberRange(int first, int last);
    void loadMemberPage(int page);
    void finishSatisfiedRequests();
    void finishAllRequests();
    void reapplyLoadedPages();

    QWidget* createMemberRow(int index);
    QWidget* createHeaderRow();
    void showMemberContextMenu(const BackendUser* user, const QPoint& globalPos);

    void filterEdited(const QString& text);
    void performSearch();
    void showSearchResults(QVector<const BackendUser*> users);
    void restorePagedMembers();

    const BackendUser* userAt(int index) const;

    BackendChannel& channel;
    Backend& backend;
    LongListWidget* memberList = nullptr;
    QTimer* searchTimer = nullptr;

    QVector<QString> memberIds;
    QVector<const BackendUser*> searchUsers;
    QVector<PendingRange> pendingRanges;
    QSet<int> loadedPages;
    QSet<int> inFlightPages;

    QString searchTerm;
    int memberCount = 0;
    int memberGeneration = 0;
    int searchGeneration = 0;
    bool searchMode = false;
};

} /* namespace Mattermost */
