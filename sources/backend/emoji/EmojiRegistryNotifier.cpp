/*
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#include "EmojiRegistryNotifier.h"

namespace Mattermost {

EmojiRegistryNotifier& EmojiRegistryNotifier::instance()
{
    static EmojiRegistryNotifier notifier;
    return notifier;
}

} // namespace Mattermost
