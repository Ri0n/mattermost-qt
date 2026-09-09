#pragma once

// Compatibility include for the read-boundary observer in ChatLogWidget.
// Cursor state is owned directly by the shared FollowingModel; there is no
// separate ReadCursorService object or storage.
#include "FollowingModel.h"

namespace Mattermost {
using ReadCursorService = FollowingModel;
}
