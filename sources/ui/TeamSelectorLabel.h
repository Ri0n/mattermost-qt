#pragma once

#include <QToolButton>
#include <QString>

class QShowEvent;

namespace Mattermost {

class ChannelTree;

/**
 * Compact active-team selector used by the left sidebar.
 *
 * ChannelTree keeps one logical root item per Mattermost team. The selector
 * installs the active TeamItem as QTreeView's root index, so that logical root
 * remains available to the existing category/DnD code but is never rendered;
 * its categories are the visual top level of the sidebar.
 */
class TeamSelectorLabel final : public QToolButton
{
    Q_OBJECT

public:
    explicit TeamSelectorLabel(QWidget* parent = nullptr);

    QString activeTeamId() const { return activeTeamId_; }

    /** Switch the selector belonging to context's window, if one exists. */
    static bool activateTeam(QWidget* context, const QString& teamId);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void attachTree();
    void refreshTeams();
    bool setActiveTeam(const QString& teamId, bool persist);
    bool hasTeamRoot(const QString& teamId) const;
    void showTeamMenu();
    void addAnotherTeam();

    ChannelTree* tree_ = nullptr;
    QString activeTeamId_;
    QString preferredTeamId_;
    QString pendingJoinedTeamId_;
};

} // namespace Mattermost
