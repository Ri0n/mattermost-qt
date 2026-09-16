#pragma once

#include <algorithm>

namespace Mattermost {

struct PostSelectionRange {
    int first = -1;
    int last = -1;

    bool isValid() const { return first >= 0 && last >= first; }
    bool contains(int index) const { return isValid() && index >= first && index <= last; }
};

inline PostSelectionRange postSelectionRange(int anchorIndex, int currentIndex)
{
    if (anchorIndex < 0 || currentIndex < 0) {
        return {};
    }
    return {std::min(anchorIndex, currentIndex), std::max(anchorIndex, currentIndex)};
}

inline bool postSelectionShouldExit(int selectedCount)
{
    return selectedCount <= 0;
}

} // namespace Mattermost
