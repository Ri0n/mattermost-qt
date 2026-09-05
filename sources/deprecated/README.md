# Deprecated chat-log implementation

This directory is an intentional compile-time quarantine for the previous sparse timeline/list implementation.

- Files here are excluded from the normal application source glob.
- Active code must not include headers from this directory.
- There are intentionally no forwarding headers at the old paths.
- A compile error caused by a missing old header is a migration task, not something to hide with a compatibility shim.

The replacement architecture is documented in `docs/long-list-architecture.md` and starts with `sources/widgets/LongListWidget.*`.
