/**
 * @file UserSearchDialog.cpp
 * @brief Server-backed user search without preloading the Mattermost directory.
 */

#include "UserSearchDialog.h"

#include <utility>

#include <QPointer>

#include "backend/Backend.h"
#include "backend/types/BackendUser.h"
#include "ui_FilterListDialog.h"

namespace Mattermost {

namespace {

constexpr int SearchDelayMs = 250;
constexpr int MinimumSearchLength = 2;

} // namespace

UserSearchDialog::UserSearchDialog(Backend& backend,
                                   const FilterListDialogConfig& cfg,
                                   UserSearchOptions options,
                                   const QSet<QString>& disabledUserIds,
                                   QWidget* parent)
    : UserListDialog(parent)
    , backend(backend)
    , cfg(cfg)
    , searchOptions(std::move(options))
    , disabledUserIds(disabledUserIds)
{
    create(cfg, {}, {QStringLiteral("Full Name"), QStringLiteral("Status")});
    ui->filterLineEdit->setPlaceholderText(QStringLiteral("Type at least 2 characters"));

    searchTimer.setSingleShot(true);
    searchTimer.setInterval(SearchDelayMs);
    connect(&searchTimer, &QTimer::timeout, this, &UserSearchDialog::performSearch);
    connect(ui->filterLineEdit, &QLineEdit::textEdited,
            this, &UserSearchDialog::filterEdited);
}

void UserSearchDialog::filterEdited(const QString& text)
{
    searchTimer.stop();
    searchTerm = text.trimmed();
    ++searchGeneration;

    if (searchTerm.size() < MinimumSearchLength) {
        showUsers({});
        return;
    }

    searchTimer.start();
}

void UserSearchDialog::performSearch()
{
    UserSearchOptions options = searchOptions;
    options.term = searchTerm;
    const int generation = searchGeneration;
    QPointer<UserSearchDialog> guard(this);

    UserProfileService::instance(backend).searchUsers(
        options, [guard, generation](QVector<const BackendUser*> users) mutable {
            if (!guard || generation != guard->searchGeneration) {
                return;
            }

            QStringList userIds;
            userIds.reserve(users.size());
            for (const BackendUser* user : users) {
                if (user) {
                    userIds.push_back(user->id);
                }
            }

            UserProfileService::instance(guard->backend).ensureStatuses(
                userIds, [guard, generation, users = std::move(users)]() mutable {
                    if (!guard || generation != guard->searchGeneration) {
                        return;
                    }
                    guard->showUsers(users);
                });
        });
}

void UserSearchDialog::showUsers(const QVector<const BackendUser*>& users)
{
    std::set<UserListEntry> entries;
    const QString currentUserId = backend.getLoginUser().id;

    for (const BackendUser* user : users) {
        if (!user || user->id == currentUserId) {
            continue;
        }
        entries.emplace(user, disabledUserIds.contains(user->id));
    }

    dataToItemMap.clear();
    ui->tableWidget->clearContents();
    create(cfg, entries, {QStringLiteral("Full Name"), QStringLiteral("Status")});
}

} // namespace Mattermost
