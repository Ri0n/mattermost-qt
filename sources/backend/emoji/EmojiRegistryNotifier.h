/*
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#pragma once

#include <QObject>
#include <QString>

namespace Mattermost {

/** Coordinates lazy custom-emoji resolution with formatter rerendering. */
class EmojiRegistryNotifier final : public QObject
{
    Q_OBJECT
public:
    static EmojiRegistryNotifier& instance();

signals:
    /** Emitted by renderers when a syntactically valid :name: is not registered. */
    void customEmojiRequested(const QString& name);

    /** Emitted after a custom emoji image becomes available to MessageFormatter. */
    void customEmojiAdded(const QString& name);

private:
    EmojiRegistryNotifier() = default;
};

} // namespace Mattermost
