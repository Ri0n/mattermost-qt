/**
 * @file UserSearchDialog.h
 * @brief Server-backed user search without preloading the Mattermost directory.
 */

#pragma once

#include <QSet>
#include <QTimer>

#include "UserListDialog.h"
#include "backend/UserProfileService.h"

namespace Mattermost {

class Backend;

class UserSearchDialog : public UserListDialog {
public:
    UserSearchDialog(Backend& backend,
                     const FilterListDialogConfig& cfg,
                     UserSearchOptions options,
                     const QSet<QString>& disabledUserIds = {},
                     QWidget* parent = nullptr);

    const BackendUser* getSelectedUser();

public slots:
    void accept() override;

private:
    void filterEdited(const QString& text);
    void performSearch();
    void showUsers(const QVector<const BackendUser*>& serverUsers);
    bool matchesSearch(const BackendUser& user) const;

    Backend& backend;
    FilterListDialogConfig cfg;
    UserSearchOptions searchOptions;
    QSet<QString> disabledUserIds;
    QTimer searchTimer;
    QString searchTerm;
    int searchGeneration = 0;
    bool handledExistingDirect = false;
};

} // namespace Mattermost
