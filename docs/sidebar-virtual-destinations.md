# Virtual sidebar destinations and post collections

This note records the model for user-centric destinations that look like navigation entries but are
not ordinary server sidebar rows, plus the common collection semantics needed by Saved and future
message search.

## Personal

The user's self-contact/self-DM is exposed as **Personal** (`Личное`) as the first local row inside the
existing **Favorites** sidebar category.

`Personal` is a virtual navigation item, but its destination is a real canonical self-DM channel. It
resolves the logged-in user's direct channel with themselves and opens the ordinary channel/timeline
path. If that direct channel has never existed, activation creates it through Mattermost's normal
`/channels/direct` path first; there is still no fake `BackendChannel`.

Consequences:

- `Personal` has a stable local destination identity independent of a server category row;
- the real self-DM keeps the normal channel ID, post cache namespace, unread/history/thread behavior;
- the virtual row is non-draggable and never participates in sidebar category mutation payloads;
- a real self-DM row is suppressed inside Favorites so the same conversation is not shown twice there;
- if the server later exposes the self-DM as an ordinary DM row elsewhere, activating it redirects to
  `Personal` as the user-facing canonical shortcut;
- the row can use the logged-in user's avatar while keeping the label `Personal`.

`SidebarItem::VirtualDestination` and `SidebarItem::DestinationRole` keep this semantic distinction
explicit instead of pretending the local row is an ordinary server channel row.

## Saved

**Saved** (`Сохранённое`) is fundamentally different. Saved posts can originate from multiple channels
and threads, so it must not pretend to be a `BackendChannel`.

It should be modeled as a virtual destination backed by a cross-conversation post collection:

```text
Saved
  +-- post A from channel X
  +-- post B from thread Y / root R
  +-- post C from channel Z
```

Each collection entry keeps semantic origin, not a fabricated conversation coordinate:

```text
postId
channelId
rootId      optional; non-empty means the origin is a thread
```

Opening an entry resolves the real channel/thread and then performs ordinary semantic post-ID
navigation. The collection itself never invents channel page numbers or thread cursor adjacency.

## Message search

Future message search should reuse the same collection/navigation model as Saved. The difference is
lifetime and producer, not row semantics:

```text
Saved collection                 Search result collection
persistent user-selected set     ephemeral query result set
        |                                  |
        +----------- common entry ----------+
                    postId
                    channelId
                    optional rootId
                          |
                          v
              canonical conversation
                          |
                    navigate to post
```

This means search results should not be inserted into `BackendChannel::posts` as if they formed a
contiguous history window. A search endpoint proves only that those posts matched a query and their
result ordering; it does not prove adjacency in the source conversation.

The eventual shared collection layer can therefore own:

- ordered collection entries and collection-specific paging;
- lazy body resolution through `PostRepository::loadPost()`;
- origin labels/context preview;
- activation into channel versus thread based on `rootId`;
- semantic `goToPost(postId)` after the real conversation is open.

`Saved` may be represented by a fixed virtual navigation destination. Search results are normally a
transient destination created by a search action rather than a permanent sidebar row, but both should
reuse the same post-collection view/source machinery.

## Cache interaction

The persistent post cache may make Saved/Search rows paint quickly because collection entries identify
individual posts. It still receives no extra timeline authority from those collections. A cached body
can satisfy first paint and normal HTTP validation can refresh it, while channel/thread sources remain
the only owners of conversation placement.
