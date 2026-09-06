# Virtual sidebar destinations

This note records the intended model for user-centric destinations that look like sidebar entries but
are not ordinary server channel rows.

## Personal

The user's self-contact/self-DM should be exposed as **Personal** (`Личное` in the Russian UI) inside
the existing **Favorites** sidebar group.

`Personal` is a virtual navigation item, but its destination is a real canonical self-DM channel. It
should resolve the logged-in user's direct channel with themselves and then open that normal channel
through the existing channel/timeline path. The virtual item should not create a duplicate
`BackendChannel` or a second post source for the same conversation.

Consequences:

- one canonical self-DM identity and one resident/cache namespace;
- unread/history/thread behavior remains ordinary channel behavior;
- the sidebar item may have its own stable virtual item ID/icon/label while resolving to the real
  channel only when activated;
- it belongs in Favorites because it is a convenient user-facing shortcut, not a new server category.

## Saved

**Saved** (`Сохранённое`) is fundamentally different. Saved posts can originate from multiple
channels and threads, so it must not pretend to be a `BackendChannel`.

It should be modeled as a separate virtual destination with an aggregate source/navigation model:

```text
Saved virtual destination
        |
        +-- post A from channel X
        +-- post B from thread Y/root R
        +-- post C from channel Z
```

Each saved entry keeps its original semantic context (channel ID, post ID, optional root/thread ID) so
opening/navigating it can resolve the real conversation and use the normal semantic post-ID navigation
path. The aggregate list itself must not invent channel page or thread cursor authority.

The exact sidebar placement/UI for Saved can be decided separately; the important architectural rule is
that **Personal resolves to one real channel, while Saved is a cross-conversation aggregate and is not a
fake channel**.
