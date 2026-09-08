#pragma once

#include <QString>
#include <QStringList>

namespace Mattermost {

/**
 * Tracks the one semantic thread target whose logical position is still being
 * reconciled with a server window.
 *
 * A freshly estimated slot is navigation metadata only: its body may already be
 * resident, but the view must not materialize a QWidget until a server window
 * containing the same identity confirms or relocates it. A target that was
 * already mapped by cached thread state is still tracked for overlap protection,
 * but remains materializable while that cached placement is validated.
 */
class ThreadNavigationPlacement
{
public:
    struct Confirmation {
        int previousIndex = -1;
        int authoritativeIndex = -1;
        bool wasEstimated = false;

        bool isValid() const
        {
            return previousIndex >= 0 && authoritativeIndex >= 0;
        }

        bool moved() const
        {
            return isValid() && previousIndex != authoritativeIndex;
        }
    };

    void trackEstimated(const QString& postId, int index)
    {
        targetPostId = postId;
        targetIndex = index;
        estimated = !postId.isEmpty() && index >= 0;
    }

    void trackExistingProvisional(const QString& postId, int index)
    {
        targetPostId = postId;
        targetIndex = index;
        estimated = false;
    }

    bool isActive() const
    {
        return !targetPostId.isEmpty() && targetIndex >= 0;
    }

    const QString& postId() const
    {
        return targetPostId;
    }

    int index() const
    {
        return targetIndex;
    }

    bool blocksMaterialization(const QString& postId) const
    {
        return estimated && isActive() && postId == targetPostId;
    }

    Confirmation confirmExactWindow(int first, const QStringList& ids)
    {
        if (!isActive()) {
            return {};
        }

        const int offset = ids.indexOf(targetPostId);
        if (offset < 0) {
            return {};
        }

        Confirmation result;
        result.previousIndex = targetIndex;
        result.authoritativeIndex = first + offset;
        result.wasEstimated = estimated;
        clear();
        return result;
    }

    void clear()
    {
        targetPostId.clear();
        targetIndex = -1;
        estimated = false;
    }

private:
    QString targetPostId;
    int targetIndex = -1;
    bool estimated = false;
};

} // namespace Mattermost
