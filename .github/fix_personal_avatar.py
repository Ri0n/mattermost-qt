from pathlib import Path

p = Path('sources/channel-tree/ChannelTree.cpp')
s = p.read_text()
old = '''    const BackendUser& self = backend.getLoginUser();
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
'''
new = '''    const BackendUser& self = backend.getLoginUser();
    item->setStatus(self.status);
    if (!self.avatar.isNull()) {
        item->setIcon(QIcon(self.avatar));
    }

    if (!personalUserConnected) {
        personalUserConnected = true;
        if (self.avatar.isNull()) {
            backend.retrieveUserAvatar(self.id);
        }
        connect(&self, &BackendUser::onAvatarChanged, this,
                [this] { refreshPersonalItems(); });
        connect(&self, &BackendUser::onStatusChanged, this,
                [this] { refreshPersonalItems(); });
    }
'''
if old not in s:
    raise SystemExit('createPersonalItem avatar block not found')
s = s.replace(old, new, 1)
old = '''    const BackendUser& self = backendForSidebar->getLoginUser();
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
'''
new = '''    const BackendUser& self = backendForSidebar->getLoginUser();
    BackendChannel* channel = backendForSidebar->getStorage().getDirectChannelByUserId(self.id);

    for (auto it = teamToItemMap.cbegin(); it != teamToItemMap.cend(); ++it) {
        QTreeWidgetItem* row = personalItemForTeam(it.key());
        if (!row) {
            continue;
        }
        auto* personalItem = static_cast<ChannelItem*>(row);
        personalItem->setStatus(self.status);
        personalItem->setIcon(self.avatar.isNull() ? QIcon() : QIcon(self.avatar));
        row->setData(0, ItemChannelIdRole, channel ? channel->id : QString());
    }
'''
if old not in s:
    raise SystemExit('refreshPersonalItems avatar block not found')
s = s.replace(old, new, 1)
p.write_text(s)
