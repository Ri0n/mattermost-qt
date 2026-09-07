/*
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#pragma once

#include <QObject>
#include <QString>

namespace Mattermost {

/** Emits when a custom emoji becomes available to MessageFormatter. */
class EmojiRegistryNotifier final : public QObject
{
    Q_OBJECT
public:
    static EmojiRegistryNotifier& instance();

signals:
    void customEmojiAdded(const QString& name);

private:
    EmojiRegistryNotifier() = default;
};

} // namespace Mattermost
