from pathlib import Path

p = Path('sources/backend/types/BackendPost.cpp')
text = p.read_text()
old = '''\tconst bool nextDeleted = editedPost.delete_at != 0 || editedPost.isDeleted;\n\tconst bool nextHidden = editedPost.hidden || !root_id.isEmpty();\n\tconst bool changed = update_at != editedPost.update_at\n'''
new = '''\tconst bool nextDeleted = editedPost.delete_at != 0 || editedPost.isDeleted;\n\tconst bool nextHidden = editedPost.hidden || !root_id.isEmpty();\n\tconst QString nextSenderName = editedPost.sender_name.isEmpty()\n\t\t? sender_name : editedPost.sender_name;\n\tconst bool nextCurrentUserMentioned = currentUserMentioned\n\t\t|| editedPost.currentUserMentioned;\n\tconst bool changed = update_at != editedPost.update_at\n'''
if text.count(old) != 1:
    raise SystemExit(f'first fragment count={text.count(old)}')
text = text.replace(old, new, 1)
text = text.replace('\t\t|| sender_name != editedPost.sender_name\n',
                    '\t\t|| sender_name != nextSenderName\n', 1)
text = text.replace('\t\t|| currentUserMentioned != editedPost.currentUserMentioned\n',
                    '\t\t|| currentUserMentioned != nextCurrentUserMentioned\n', 1)
text = text.replace('\tsender_name = editedPost.sender_name;\n',
                    '\tsender_name = nextSenderName;\n', 1)
text = text.replace('\tcurrentUserMentioned = editedPost.currentUserMentioned;\n',
                    '\tcurrentUserMentioned = nextCurrentUserMentioned;\n', 1)
p.write_text(text)
