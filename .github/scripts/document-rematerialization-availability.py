from pathlib import Path


def replace_once(path, old, new):
    p = Path(path)
    text = p.read_text()
    if text.count(old) != 1:
        raise SystemExit(f'{path}: expected one match, found {text.count(old)}')
    p.write_text(text.replace(old, new, 1))


replace_once(
    'docs/post-source-architecture.md',
    '''Fetching an identical page twice produces no source signals. This matters because `rangeAvailable`\nand `itemsChanged` both schedule list synchronization; emitting them for an identity no-op can turn a\nboundary mismatch into a tight repeat-request loop.\n\nAn empty string remains an **unavailable logical slot**, not a fake post and not a widget placeholder.\n''',
    '''Fetching an identical page twice currently produces no source signals. This matters because\n`rangeAvailable` and `itemsChanged` both schedule list synchronization; emitting them for an identity\nno-op can turn a boundary mismatch into a tight repeat-request loop.\n\nThis identity no-op rule must remain separate from future resident-body availability. Once Phase 3 can\nevict a `BackendPost` while retaining its source ID, rematerializing that same ID must publish\n"resident body available again" even though the logical identity mapping did not change. That path\nshould be an explicit availability/rematerialization notification, not a fake identity mutation.\n\nAn empty string remains an **unavailable logical slot**, not a fake post and not a widget placeholder.\n''')

replace_once(
    'docs/post-cache-runtime.md',
    '''The shared `IndexedPostSource` only performs identity-slot mutation after a concrete source decides\nwhether placement is exact or provisional. See `post-source-architecture.md`.\n\n## Channel paging authority\n''',
    '''The shared `IndexedPostSource` only performs identity-slot mutation after a concrete source decides\nwhether placement is exact or provisional. See `post-source-architecture.md`.\n\n### Stable identity versus resident availability\n\nThe planned resident cache deliberately keeps logical source IDs after their `BackendPost` body is\nevicted. Therefore two state changes must remain independent:\n\n```text\nidentity mapping changes\n    -> itemsChanged / structural slot signals\n\nidentity unchanged, resident body rematerialized\n    -> availability/rematerialization notification only\n```\n\nCurrent no-op suppression for an identical authoritative page is correct while every mapped ID is\nresident. Phase 3 must add an explicit rematerialization availability path rather than making an\nidentical page pretend that its identity mapping changed. This preserves both request-loop protection\nand safe body eviction.\n\n## Channel paging authority\n''')

print('rematerialization availability contract documented')
