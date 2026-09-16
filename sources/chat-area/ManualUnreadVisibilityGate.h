#pragma once

#include <QString>
#include <utility>

namespace Mattermost {

/**
 * Prevents a freshly manual-unread post from being immediately consumed merely
 * because its lower edge was already visible when the command was issued.
 */
class ManualUnreadVisibilityGate
{
public:
    enum class Phase {
        Idle,
        WaitForExit,
        WaitForEntry,
    };

    void markUnread(QString postId, bool lowerEdgeVisible)
    {
        postId_ = std::move(postId);
        phase_ = postId_.isEmpty()
            ? Phase::Idle
            : (lowerEdgeVisible ? Phase::WaitForExit : Phase::WaitForEntry);
    }

    /** Returns true while the current read-progress pass must be blocked. */
    bool update(bool lowerEdgeVisible)
    {
        if (phase_ == Phase::Idle) {
            return false;
        }
        if (phase_ == Phase::WaitForExit) {
            if (!lowerEdgeVisible) {
                phase_ = Phase::WaitForEntry;
            }
            return true;
        }
        if (!lowerEdgeVisible) {
            return true;
        }
        clear();
        return false;
    }

    void clear()
    {
        postId_.clear();
        phase_ = Phase::Idle;
    }

    bool active() const { return phase_ != Phase::Idle; }
    const QString& postId() const { return postId_; }
    Phase phase() const { return phase_; }

private:
    QString postId_;
    Phase phase_ = Phase::Idle;
};

} // namespace Mattermost
