![Build status](https://github.com/Ri0n/mattermost-qt/actions/workflows/build.yaml/badge.svg?branch=master)

# mattermost-qt

A native Mattermost desktop client built with Qt.

This repository is a maintained fork of [turok-m-a/mattermost-qt](https://github.com/turok-m-a/mattermost-qt), originally created by nuclear868.

## Motivation

The official Mattermost desktop client is based on Electron and embeds the web application. `mattermost-qt` aims to provide a lightweight native alternative with lower resource usage and conventional desktop UI behavior, including native widgets, context menus and thread windows.

## Highlights

- Channels, direct messages and group messages.
- Sending, receiving, editing and deleting messages.
- Threads in separate windows, including follow/unfollow support and restoration of the previous reading position.
- File attachments, image previews and opening/downloading non-image attachments.
- Emoji and reactions, including adding a reaction by clicking an existing reaction chip.
- Markdown rendering, quoted messages and fenced code blocks with syntax highlighting.
- Pinned posts with direct `Go to message` navigation.
- Mattermost-style server-backed sidebar categories, Favorites/custom categories, mute state and drag/drop ordering.
- `Channels`, `Recent` and `Attention` sidebar views, including unread filtering and followed-thread/mention handling.
- Direct-message search and lazy server-side user discovery.
- Presence/status indicators and profile dialogs with full-resolution avatars.
- Desktop notifications with message/thread targets on supported Linux desktops, with tray notifications as a fallback.
- Reliable WebSocket reconnect/resume using Mattermost connection and sequence state, avoiding the old full-history refresh after a successful resume.
- Lazy loading of channels, user profiles, avatars and member lists instead of downloading the complete server directory at startup.
- Sparse/windowed channel and thread histories. Long conversations keep only a bounded number of materialized message widgets while older/newer regions are loaded on demand.
- Viewport anchoring and sticky-bottom behavior designed to keep the reading position stable while history, images and message layouts change.

The client is under active development and does not yet provide complete Mattermost feature parity. Voice/video calls and some less common server/plugin features are not implemented.

## Platforms and CI

The project uses C++17 and CMake 3.16 or newer.

Current CI covers:

- Qt 6.10 on Ubuntu;
- Qt 5 on Ubuntu 22.04;
- Qt 5 on Fedora;
- Qt 6.10 with MSVC on Windows;
- Ubuntu 26.04 package builds.

Unit tests are run in the CI matrix. Compiler warnings can be promoted to errors with `-DWARNINGS_AS_ERRORS=ON`.

A separate manually triggered [Packages workflow](https://github.com/Ri0n/mattermost-qt/actions/workflows/packages.yml) builds:

- DEB packages for Ubuntu 22.04 / Qt 5;
- DEB packages for Ubuntu 24.04 / Qt 6;
- DEB packages for Ubuntu 26.04 / Qt 6;
- a Windows x64 NSIS installer using Qt 6 and MSVC.

## Build requirements

Required Qt modules:

- Widgets
- Network
- WebSockets

Qt 6 is preferred by default. Qt 5 is still supported and can be selected explicitly with `-DQT_6=OFF`.

On Linux, Qt DBus is optional and enables native freedesktop notifications when available.

### Ubuntu / Debian

For Qt 6:

```bash
sudo apt install build-essential cmake qt6-base-dev qt6-websockets-dev
```

For Qt 5:

```bash
sudo apt install build-essential cmake qtbase5-dev libqt5websockets5-dev
```

## Build instructions

It is recommended to use an out-of-tree build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

To force a Qt 5 build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DQT_6=OFF
cmake --build build --parallel
```

Tests can be enabled explicitly with:

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### Windows

Windows is actively built in CI with Qt 6.10 and MSVC. A normal Qt/CMake build works with the same source tree; the packaging workflow additionally uses Qt deployment support and CPack/NSIS to produce a self-contained installer.

## Running

From the build directory, start:

```bash
./mattermost-qt
```

A login dialog appears on first start. After a successful login, the server URL, username and login token are stored using `QSettings`; the password itself is not saved. On Linux the settings normally live under `~/.config/mattermost-native/Mattermost.conf`.

The saved token is currently not encrypted, so the settings file should be treated as sensitive.

## Contributing

Bug reports and pull requests are welcome. The project is maintained as a lightweight native Mattermost client, so changes that improve compatibility, reliability, resource usage and normal desktop usability are especially useful.
