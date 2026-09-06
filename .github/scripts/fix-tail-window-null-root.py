from pathlib import Path

p = Path('sources/backend/PostCacheStore.cpp')
text = p.read_text()
old = '''bool PostCacheStore::storeTailWindow(const QString& channelId,
                                         const QString& rootId,
                                         const QStringList& chronologicalPostIds)
{
    if (!hasAccount() || channelId.isEmpty()
        || !isChannelEligible(channelId, nowMs())) {
        return false;
    }

    QJsonArray ids;
'''
new = '''bool PostCacheStore::storeTailWindow(const QString& channelId,
                                         const QString& rootId,
                                         const QStringList& chronologicalPostIds)
{
    if (!hasAccount() || channelId.isEmpty()
        || !isChannelEligible(channelId, nowMs())) {
        return false;
    }

    // QString() is a null string and QSql binds it as SQL NULL. Main-channel
    // provenance deliberately uses the non-null empty string as its root key.
    const QString storageRootId = rootId.isNull() ? QStringLiteral("") : rootId;

    QJsonArray ids;
'''
if text.count(old) != 1:
    raise SystemExit(f'store header match count={text.count(old)}')
text = text.replace(old, new, 1)
old = '''    query.addBindValue(accountId);
    query.addBindValue(channelId);
    query.addBindValue(rootId);
    query.addBindValue(nowMs());
'''
new = '''    query.addBindValue(accountId);
    query.addBindValue(channelId);
    query.addBindValue(storageRootId);
    query.addBindValue(nowMs());
'''
if text.count(old) != 1:
    raise SystemExit(f'store bind match count={text.count(old)}')
text = text.replace(old, new, 1)
old = '''QJsonObject PostCacheStore::loadTailWindow(const QString& channelId,
                                           const QString& rootId,
                                           int limit)
{
    if (!hasAccount() || channelId.isEmpty() || limit <= 0
        || !isChannelEligible(channelId, nowMs())) {
        return {};
    }

    QSqlQuery window(database);
'''
new = '''QJsonObject PostCacheStore::loadTailWindow(const QString& channelId,
                                           const QString& rootId,
                                           int limit)
{
    if (!hasAccount() || channelId.isEmpty() || limit <= 0
        || !isChannelEligible(channelId, nowMs())) {
        return {};
    }

    const QString storageRootId = rootId.isNull() ? QStringLiteral("") : rootId;
    QSqlQuery window(database);
'''
if text.count(old) != 1:
    raise SystemExit(f'load header match count={text.count(old)}')
text = text.replace(old, new, 1)
old = '''    window.addBindValue(accountId);
    window.addBindValue(channelId);
    window.addBindValue(rootId);
'''
new = '''    window.addBindValue(accountId);
    window.addBindValue(channelId);
    window.addBindValue(storageRootId);
'''
if text.count(old) != 1:
    raise SystemExit(f'load bind match count={text.count(old)}')
text = text.replace(old, new, 1)
p.write_text(text)
print('normalized null root IDs at SQLite tail-window boundary')
