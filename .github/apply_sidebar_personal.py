from pathlib import Path
import re


def replace_once(path, old, new):
    p = Path(path)
    text = p.read_text()
    if old not in text:
        raise SystemExit(f"pattern not found in {path}: {old[:80]!r}")
    p.write_text(text.replace(old, new, 1))


def regex_once(path, pattern, replacement):
    p = Path(path)
    text = p.read_text()
    new_text, count = re.subn(pattern, replacement, text, count=1, flags=re.S)
    if count != 1:
        raise SystemExit(f"regex matched {count} times in {path}: {pattern!r}")
    p.write_text(new_text)


# Sidebar semantic contract.
replace_once(
    "sources/channel-tree/SidebarItem.h",
    """enum Kind {\n    Unknown = 0,\n    Team,\n    Category,\n    Channel,\n    Thread,\n};\n\n/**\n * Shared model roles for sidebar rows.\n""",
    """enum Kind {\n    Unknown = 0,\n    Team,\n    Category,\n    Channel,\n    Thread,\n    VirtualDestination,\n};\n\n/** Local destinations that are not ordinary server sidebar rows. */\nenum Destination {\n    NoDestination = 0,\n    PersonalDestination,\n    SavedDestination,\n};\n\n/**\n * Shared model roles for sidebar rows.\n""",
)
replace_once(
    "sources/channel-tree/SidebarItem.h",
    """    ChannelIdRole,\n    ThreadIdRole,\n};\n""",
    """    ChannelIdRole,\n    ThreadIdRole,\n    DestinationRole,\n};\n""",
)

# Virtual destinations are visually conversation rows, while remaining semantically distinct.
replace_once(
    "sources/channel-tree/ChannelItemDelegate.cpp",
    """int channelType(const QModelIndex& index)\n{\n    return index.data(SidebarItem::ChannelTypeRole).toInt();\n}\n\n} // namespace\n""",
    """int channelType(const QModelIndex& index)\n{\n    return index.data(SidebarItem::ChannelTypeRole).toInt();\n}\n\nbool isConversationRow(const QModelIndex& index)\n{\n    const int kind = index.data(SidebarItem::KindRole).toInt();\n    return kind == SidebarItem::Channel || kind == SidebarItem::VirtualDestination;\n}\n\n} // namespace\n""",
)
replace_once(
    "sources/channel-tree/ChannelItemDelegate.cpp",
    """    if (index.data(SidebarItem::KindRole).toInt() == SidebarItem::Channel) {\n        hint.setHeight(ChannelRowHeight);\n    }\n""",
    """    if (isConversationRow(index)) {\n        hint.setHeight(ChannelRowHeight);\n    }\n""",
)
replace_once(
    "sources/channel-tree/ChannelItemDelegate.cpp",
    """    if (index.data(SidebarItem::KindRole).toInt() != SidebarItem::Channel) {\n        QStyledItemDelegate::paint(painter, option, index);\n        return;\n    }\n""",
    """    if (!isConversationRow(index)) {\n        QStyledItemDelegate::paint(painter, option, index);\n        return;\n    }\n""",
)

# Backend direct-channel creation callback so a first-ever self-DM can be opened immediately.
replace_once(
    "sources/backend/Backend.h",
    """\t//create a direct channel with given user (/channels/direct)\n\tvoid createDirectChannel (const BackendUser& user);\n""",
    """\t//create a direct channel with given user (/channels/direct)\n\tvoid createDirectChannel(const BackendUser& user,\n\t                         std::function<void(BackendChannel&)> callback = {});\n""",
)
regex_once(
    "sources/backend/Backend.cpp",
    r"void Backend::createDirectChannel \(const BackendUser& user\)\n\{.*?\n\}\n\nvoid Backend::retrieveChannelPosts",
    """void Backend::createDirectChannel(const BackendUser& user,\n                                  std::function<void(BackendChannel&)> callback)\n{\n    QJsonArray json {getLoginUser().id, user.id};\n\n    NetworkRequest request(\"channels/direct\");\n    httpConnector.post(request, json, HttpResponseCallback(\n        [this, callback = std::move(callback)](const QJsonDocument& doc) mutable {\n            BackendChannel* channel = storage.addDirectChannel(doc.object());\n            if (!channel) {\n                return;\n            }\n\n            LOG_DEBUG(\"\\tNew Channel added: \" << channel->id << \" \" << channel->display_name);\n            emit storage.directChannels.onNewChannel(*channel);\n\n            // Keep the server-side DM preference in sync with the existing\n            // create-direct-channel behavior used by the official client.\n            updateUserPreferences(BackendUserPreferences {\n                \"direct_channel_show\", channel->name, \"true\"});\n\n            if (callback) {\n                callback(*channel);\n            }\n        }));\n}\n\nvoid Backend::retrieveChannelPosts""",
)

# ChannelTree contract and helpers.
replace_once(
    "sources/channel-tree/ChannelTree.h",
    """    static constexpr ItemKind ChannelItemKind = SidebarItem::Channel;\n""",
    """    static constexpr ItemKind ChannelItemKind = SidebarItem::Channel;\n    static constexpr ItemKind VirtualDestinationItemKind = SidebarItem::VirtualDestination;\n""",
)
replace_once(
    "sources/channel-tree/ChannelTree.h",
    """    static constexpr ItemRole ItemChannelTypeRole = SidebarItem::ChannelTypeRole;\n""",
    """    static constexpr ItemRole ItemChannelTypeRole = SidebarItem::ChannelTypeRole;\n    static constexpr ItemRole ItemChannelIdRole = SidebarItem::ChannelIdRole;\n    static constexpr ItemRole ItemDestinationRole = SidebarItem::DestinationRole;\n""",
)
replace_once(
    "sources/channel-tree/ChannelTree.h",
    """\tChannelItem* createChannelItem(Backend& backend, TeamItem& teamItem,\n\t                               QTreeWidgetItem& categoryItem, BackendChannel& channel);\n\tChatArea* ensureChatArea(QTreeWidgetItem* item);\n\tvoid activateChannelItem(QTreeWidgetItem* item);\n""",
    """\tChannelItem* createChannelItem(Backend& backend, TeamItem& teamItem,\n\t                               QTreeWidgetItem& categoryItem, BackendChannel& channel);\n    ChannelItem* createPersonalItem(Backend& backend, TeamItem& teamItem,\n                                    QTreeWidgetItem& categoryItem);\n    QTreeWidgetItem* personalItemForTeam(const QString& teamId) const;\n    void refreshPersonalItems();\n\tChatArea* ensureChatArea(QTreeWidgetItem* item);\n\tvoid activateChannelItem(QTreeWidgetItem* item);\n    void activateVirtualDestination(QTreeWidgetItem* item);\n""",
)
replace_once(
    "sources/channel-tree/ChannelTree.h",
    """\tbool\t\t\t\t\t\t\t\trenderingSidebar;\n};\n""",
    """\tbool\t\t\t\t\t\t\t\trenderingSidebar;\n    bool                                personalUserConnected = false;\n};\n""",
)

# ChannelTree implementation.
replace_once(
    "sources/channel-tree/ChannelTree.cpp",
    """#include <QHeaderView>\n#include <QStackedWidget>\n""",
    """#include <QHeaderView>\n#include <QPointer>\n#include <QStackedWidget>\n""",
)
replace_once(
    "sources/channel-tree/ChannelTree.cpp",
    """    auto& sidebar = SidebarService::instance(backend);\n    for (const auto& categoryId : state.order) {\n""",
    """    auto& sidebar = SidebarService::instance(backend);\n    BackendChannel* personalChannel = backend.getStorage().getDirectChannelByUserId(\n        backend.getLoginUser().id);\n    for (const auto& categoryId : state.order) {\n""",
)
replace_once(
    "sources/channel-tree/ChannelTree.cpp",
    """        QTreeWidgetItem* categoryItem = createCategoryItem(\n            teamItem, category->id, categoryDisplayName(*category), category->collapsed);\n\n        const QStringList visibleIds = sidebar.visibleChannelIds(*category);\n        for (const auto& channelId : visibleIds) {\n            BackendChannel* channel = backend.getStorage().getChannelById(channelId);\n""",
    """        QTreeWidgetItem* categoryItem = createCategoryItem(\n            teamItem, category->id, categoryDisplayName(*category), category->collapsed);\n\n        const bool favorites = category->type == QStringLiteral(\"favorites\");\n        if (favorites) {\n            createPersonalItem(backend, teamItem, *categoryItem);\n        }\n\n        const QStringList visibleIds = sidebar.visibleChannelIds(*category);\n        for (const auto& channelId : visibleIds) {\n            BackendChannel* channel = backend.getStorage().getChannelById(channelId);\n            if (favorites && personalChannel && channel == personalChannel) {\n                // Personal is the canonical user-facing row inside Favorites.\n                // Do not render the same self-DM twice in that category.\n                continue;\n            }\n""",
)

insert_anchor = """ChannelItem* ChannelTree::createChannelItem(Backend& backend, TeamItem& teamItem,\n                                            QTreeWidgetItem& categoryItem, BackendChannel& channel)\n"""
p = Path("sources/channel-tree/ChannelTree.cpp")
text = p.read_text()
if insert_anchor not in text:
    raise SystemExit("createChannelItem anchor not found")
personal_impl = r'''ChannelItem* ChannelTree::createPersonalItem(Backend& backend, TeamItem& teamItem,
                                             QTreeWidgetItem& categoryItem)
{
    auto* item = new DirectChannelItem(backend, nullptr);
    categoryItem.addChild(item);
    item->setData(0, ItemKindRole, VirtualDestinationItemKind);
    item->setData(0, ItemIdRole, QStringLiteral("virtual:personal"));
    item->setData(0, ItemTeamIdRole, teamItem.teamId);
    item->setData(0, ItemDestinationRole, SidebarItem::PersonalDestination);
    item->setData(0, ItemChannelTypeRole, BackendChannel::directChannel);
    item->setData(0, Qt::UserRole, QVariant::fromValue(static_cast<ChatArea*>(nullptr)));
    item->setFlags(item->flags()
                   & ~(Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled | Qt::ItemIsEditable));
    item->setLabel(tr("Personal"));

    if (BackendChannel* channel = backend.getStorage().getDirectChannelByUserId(
            backend.getLoginUser().id)) {
        item->setData(0, ItemChannelIdRole, channel->id);
    }

    const BackendUser& self = backend.getLoginUser();
    item->setIcon(self.avatar.isNull()
        ? QIcon(QStringLiteral(":/img/user-icon.png"))
        : QIcon(self.avatar));

    if (!personalUserConnected) {
        personalUserConnected = true;
        if (self.avatar.isNull()) {
            backend.retrieveUserAvatar(self.id);
        }
        connect(&self, &BackendUser::onAvatarChanged, this,
                [this] { refreshPersonalItems(); });
    }
    return item;
}

QTreeWidgetItem* ChannelTree::personalItemForTeam(const QString& teamId) const
{
    TeamItem* teamItem = teamToItemMap.value(teamId, nullptr);
    if (!teamItem) {
        return nullptr;
    }

    for (int categoryIndex = 0; categoryIndex < teamItem->childCount(); ++categoryIndex) {
        QTreeWidgetItem* category = teamItem->child(categoryIndex);
        for (int rowIndex = 0; category && rowIndex < category->childCount(); ++rowIndex) {
            QTreeWidgetItem* row = category->child(rowIndex);
            if (row
                && row->data(0, ItemKindRole).toInt() == VirtualDestinationItemKind
                && row->data(0, ItemDestinationRole).toInt() == SidebarItem::PersonalDestination) {
                return row;
            }
        }
    }
    return nullptr;
}

void ChannelTree::refreshPersonalItems()
{
    if (!backendForSidebar) {
        return;
    }

    const BackendUser& self = backendForSidebar->getLoginUser();
    const QIcon icon = self.avatar.isNull()
        ? QIcon(QStringLiteral(":/img/user-icon.png"))
        : QIcon(self.avatar);
    BackendChannel* channel = backendForSidebar->getStorage().getDirectChannelByUserId(self.id);

    for (auto it = teamToItemMap.cbegin(); it != teamToItemMap.cend(); ++it) {
        QTreeWidgetItem* row = personalItemForTeam(it.key());
        if (!row) {
            continue;
        }
        static_cast<ChannelItem*>(row)->setIcon(icon);
        row->setData(0, ItemChannelIdRole, channel ? channel->id : QString());
    }
}

'''
p.write_text(text.replace(insert_anchor, personal_impl + insert_anchor, 1))

replace_once(
    "sources/channel-tree/ChannelTree.cpp",
    """ChatArea* ChannelTree::ensureChatArea(QTreeWidgetItem* item)\n{\n    if (!item || !backendForSidebar || !chatAreaStackedWidget\n        || item->data(0, ItemKindRole).toInt() != ChannelItemKind) {\n        return nullptr;\n    }\n\n    if (ChatArea* existing = item->data(0, Qt::UserRole).value<ChatArea*>()) {\n        return existing;\n    }\n\n    const QString channelId = item->data(0, ItemIdRole).toString();\n""",
    """ChatArea* ChannelTree::ensureChatArea(QTreeWidgetItem* item)\n{\n    if (!item || !backendForSidebar || !chatAreaStackedWidget) {\n        return nullptr;\n    }\n\n    const int kind = item->data(0, ItemKindRole).toInt();\n    if (kind != ChannelItemKind && kind != VirtualDestinationItemKind) {\n        return nullptr;\n    }\n\n    if (ChatArea* existing = item->data(0, Qt::UserRole).value<ChatArea*>()) {\n        return existing;\n    }\n\n    QString channelId = kind == ChannelItemKind\n        ? item->data(0, ItemIdRole).toString()\n        : item->data(0, ItemChannelIdRole).toString();\n    if (channelId.isEmpty() && kind == VirtualDestinationItemKind\n        && item->data(0, ItemDestinationRole).toInt() == SidebarItem::PersonalDestination) {\n        if (BackendChannel* personal = backendForSidebar->getStorage().getDirectChannelByUserId(\n                backendForSidebar->getLoginUser().id)) {\n            channelId = personal->id;\n            item->setData(0, ItemChannelIdRole, channelId);\n        }\n    }\n""",
)

# Replace activation entry with virtual routing and ordinary-self-DM redirect.
replace_once(
    "sources/channel-tree/ChannelTree.cpp",
    """void ChannelTree::activateChannelItem(QTreeWidgetItem* item)\n{\n    if (!item || item->data(0, ItemKindRole).toInt() != ChannelItemKind) {\n        return;\n    }\n\n    ChatArea* newPage = ensureChatArea(item);\n""",
    """void ChannelTree::activateChannelItem(QTreeWidgetItem* item)\n{\n    if (!item) {\n        return;\n    }\n\n    const int kind = item->data(0, ItemKindRole).toInt();\n    if (kind == VirtualDestinationItemKind) {\n        activateVirtualDestination(item);\n        return;\n    }\n    if (kind != ChannelItemKind) {\n        return;\n    }\n\n    if (backendForSidebar) {\n        BackendChannel* personal = backendForSidebar->getStorage().getDirectChannelByUserId(\n            backendForSidebar->getLoginUser().id);\n        if (personal && item->data(0, ItemIdRole).toString() == personal->id) {\n            if (QTreeWidgetItem* alias = personalItemForTeam(\n                    item->data(0, ItemTeamIdRole).toString())) {\n                setCurrentItem(alias);\n                return;\n            }\n        }\n    }\n\n    ChatArea* newPage = ensureChatArea(item);\n""",
)

# Insert virtual activation before legacy list stubs.
activate_anchor = """void ChannelTree::addGroupChannelsList (Backend& backend)\n"""
p = Path("sources/channel-tree/ChannelTree.cpp")
text = p.read_text()
if activate_anchor not in text:
    raise SystemExit("addGroupChannelsList anchor not found")
virtual_impl = r'''void ChannelTree::activateVirtualDestination(QTreeWidgetItem* item)
{
    if (!item || !backendForSidebar
        || item->data(0, ItemKindRole).toInt() != VirtualDestinationItemKind
        || item->data(0, ItemDestinationRole).toInt() != SidebarItem::PersonalDestination) {
        return;
    }

    BackendChannel* channel = backendForSidebar->getStorage().getDirectChannelByUserId(
        backendForSidebar->getLoginUser().id);
    if (!channel) {
        const QString teamId = item->data(0, ItemTeamIdRole).toString();
        QPointer<ChannelTree> guard(this);
        backendForSidebar->createDirectChannel(backendForSidebar->getLoginUser(),
            [guard, teamId](BackendChannel& created) {
                if (!guard) {
                    return;
                }
                QTreeWidgetItem* currentPersonal = guard->personalItemForTeam(teamId);
                if (!currentPersonal) {
                    return;
                }
                currentPersonal->setData(0, ItemChannelIdRole, created.id);
                guard->refreshPersonalItems();
                // Do not steal focus if the user navigated elsewhere while the
                // first-ever self-DM was being created.
                if (guard->currentItem() == currentPersonal) {
                    guard->activateVirtualDestination(currentPersonal);
                }
            });
        return;
    }

    item->setData(0, ItemChannelIdRole, channel->id);
    ChatArea* newPage = ensureChatArea(item);
    if (!newPage) {
        return;
    }

    if (newPage == getCurrentPage()) {
        if (!newPage->isVisible()) {
            chatAreaStackedWidget->setCurrentWidget(newPage);
        }
        newPage->onActivate();
        return;
    }

    if (ChatArea* currentPage = getCurrentPage()) {
        currentPage->onDeactivate();
    }
    chatAreaStackedWidget->setCurrentWidget(newPage);
    newPage->onActivate();
}

'''
p.write_text(text.replace(activate_anchor, virtual_impl + activate_anchor, 1))

# Delegate regression coverage.
p = Path("tests/SidebarItemDelegateTest.cpp")
text = p.read_text()
text = text.replace(
    """    static QImage renderChannel(int channelType, const QString& presence)\n    {\n        QStandardItemModel model;\n        auto* item = new QStandardItem(QStringLiteral(\"conversation\"));\n        item->setData(SidebarItem::Channel, SidebarItem::KindRole);\n""",
    """    static QImage renderItem(SidebarItem::Kind kind, int channelType, const QString& presence)\n    {\n        QStandardItemModel model;\n        auto* item = new QStandardItem(QStringLiteral(\"conversation\"));\n        item->setData(kind, SidebarItem::KindRole);\n""",
    1)
text = text.replace("renderChannel(", "renderItem(SidebarItem::Channel, ")
marker = """    void rendersPresenceForDirectMessage()\n    {\n        const auto withPresence = renderItem(SidebarItem::Channel, BackendChannel::directChannel,\n                                                QStringLiteral(\"online\"));\n        const auto withoutPresence = renderItem(SidebarItem::Channel, BackendChannel::directChannel, QString());\n        QVERIFY(withPresence != withoutPresence);\n    }\n"""
if marker not in text:
    raise SystemExit("sidebar delegate test marker not found")
text = text.replace(marker, marker + """\n    void rendersVirtualDestinationAsConversationRow()\n    {\n        const auto channel = renderItem(SidebarItem::Channel,\n                                        BackendChannel::directChannel, QString());\n        const auto destination = renderItem(SidebarItem::VirtualDestination,\n                                            BackendChannel::directChannel, QString());\n        QCOMPARE(destination, channel);\n    }\n""", 1)
p.write_text(text)

# Expand the architecture note to include the Saved/Search common collection contract.
Path("docs/sidebar-virtual-destinations.md").write_text(r'''# Virtual sidebar destinations and post collections

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
''')

print("sidebar Personal patch applied")
