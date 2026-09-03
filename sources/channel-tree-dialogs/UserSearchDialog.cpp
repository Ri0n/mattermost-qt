/**
 * @file UserSearchDialog.cpp
 * @brief Server-backed user search without preloading the Mattermost directory.
 */

#include "UserSearchDialog.h"

#include <utility>

#include <QPointer>

#include "backend/Backend.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendUser.h"
#include "navigation/AppNavigationService.h"
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
    ui->filterLineEdit->setPlaceholderText(QStringLiteral("Filter DMs or search users"));

    searchTimer.setSingleShot(true);
    searchTimer.setInterval(SearchDelayMs);
    connect(&searchTimer, &QTimer::timeout, this, &UserSearchDialog::performSearch);
    connect(ui->filterLineEdit, &QLineEdit::textEdited,
            this, &UserSearchDialog::filterEdited);

    // The dialog is also a fast switcher for conversations already present in
    // the Direct Messages category. Show those immediately; remote discovery
    // only augments this list once the user starts typing.
    showUsers({});
}

const BackendUser* UserSearchDialog::getSelectedUser()
{
    return handledExistingDirect ? nullptr : UserListDialog::getSelectedUser();
}

void UserSearchDialog::accept()
{
    handledExistingDirect = false;
    const BackendUser* user = UserListDialog::getSelectedUser();
    if (user) {
        if (BackendChannel* channel = backend.getStorage().getDirectChannelByUserId(user->id)) {
            handledExistingDirect = true;
            AppNavigationService::instance(backend).openChannel(channel->id);
        }
    }
    QDialog::accept();
}

bool UserSearchDialog::matchesSearch(const BackendUser& user) const
{
    if (searchTerm.isEmpty()) {
        return true;
    }
    return user.getDisplayName().contains(searchTerm, Qt::CaseInsensitive)
        || user.username.contains(searchTerm, Qt::CaseInsensitive)
        || user.nickname.contains(searchTerm, Qt::CaseInsensitive)
        || user.email.contains(searchTerm, Qt::CaseInsensitive);
}

void UserSearchDialog::filterEdited(const QString& text)
{
    searchTimer.stop();
    searchTerm = text.trimmed();
    ++searchGeneration;

    // Existing DMs are local and should react immediately, even for one
    // character. This is deliberately independent of the remote-search
    // threshold used to protect the server from noisy type-ahead traffic.
    showUsers({});

    if (searchTerm.size() >= MinimumSearchLength) {
        searchTimer.start();
    }
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

void UserSearchDialog::showUsers(const QVector<const BackendUser*>& serverUsers)
{
    std::set<UserListEntry> entries;
    QSet<QString> addedUserIds;
    const QString currentUserId = backend.getLoginUser().id;

    // First mirror the actual Direct Messages collection already visible in
    // the sidebar. These rows are available synchronously and selecting one
    // opens the existing conversation instead of POSTing another DM create.
    const auto& directChannels = backend.getStorage().directChannelsByUser;
    for (auto it = directChannels.constBegin(); it != directChannels.constEnd(); ++it) {
        const QString userId = it.key();
        const BackendUser* user = backend.getStorage().getUserById(userId);
        if (!user || user->id == currentUserId || !matchesSearch(*user)) {
            continue;
        }
        entries.emplace(user, false);
        addedUserIds.insert(user->id);
    }

    // Server search augments the local DM set with people with whom there is no
    // currently materialized conversation. Deduplicate by Mattermost user ID.
    for (const BackendUser* user : serverUsers) {
        if (!user || user->id == currentUserId || addedUserIds.contains(user->id)
            || !matchesSearch(*user)) {
            continue;
        }
        entries.emplace(user, disabledUserIds.contains(user->id));
        addedUserIds.insert(user->id);
    }

    dataToItemMap.clear();
    ui->tableWidget->clearContents();
    create(cfg, entries, {QStringLiteral("Full Name"), QStringLiteral("Status")});
}

} // namespace Mattermost
