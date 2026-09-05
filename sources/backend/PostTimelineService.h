#pragma once

#include "PostRepository.h"

namespace Mattermost {

// Transitional source compatibility while the remaining post sources are
// renamed mechanically. There is no PostTimelineService object or implementation;
// all calls resolve to the single PostRepository instance.
using PostTimelineService = PostRepository;

} // namespace Mattermost
