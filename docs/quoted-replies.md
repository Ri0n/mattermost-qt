# Quoted replies

Mattermost-Qt implements an inline quoted reply independently from Mattermost thread membership.

## Wire format

A quoted reply is an otherwise normal Mattermost post whose `props` object contains:

```json
{
  "mattermost_qt_reply_to_post_id": "<post-id>"
}
```

`mattermost_qt_reply_to_post_id` is a string containing the exact ID of the post being quoted.
The constant is declared in `sources/backend/PostProps.h` as `PostProps::ReplyToPostId`.

The quoted post text is deliberately **not** copied into the new post's `message`. This avoids duplicate rendered quotes in Mattermost-Qt and keeps the reference authoritative: the quoted content is resolved from the referenced post ID.

## Relationship to threads

The reply property is orthogonal to Mattermost's `root_id`:

- `root_id` keeps its native Mattermost meaning: the post belongs to a thread rooted at that post ID.
- `props.mattermost_qt_reply_to_post_id` means only: render this post with an inline quote that references the specified post.

Consequently:

- a quoted reply in a normal channel normally has an empty `root_id`;
- a quoted reply sent from a thread window keeps that thread's `root_id` and also carries `mattermost_qt_reply_to_post_id`;
- quoted reply handling must never synthesize or replace `root_id` merely to represent the quote.

This separation is required so quoted replies inside existing threads do not accidentally create nested or unrelated thread semantics.

## Client behaviour

When the user chooses **Reply** from a post context menu, the composer stores the target post ID as a dynamic property with the same key. The composer preview is UI-only; the persistent reference is written into post `props` when the message is sent.

When rendering a post, `PostWidget` first looks for `mattermost_qt_reply_to_post_id` in `props`. If the referenced post is already materialized in the channel it is rendered immediately with `PostQuoteFrame`. Otherwise the exact post is loaded through `PostRepository::loadPost()` and the quote frame is inserted after retrieval.

Clicking the rendered quote navigates to the referenced post through `ChatArea::goToPost(postId)`, so sparse timeline materialization remains centralized in the normal navigation path.

The older **Reply in thread** action is separate and continues to use Mattermost's native thread mechanism.

## Interoperability

The property is Mattermost-Qt-specific metadata. Other clients that ignore unknown post props still display the post's normal `message`, attachments, reactions, and thread membership; they simply do not render the additional inline quote.

Do not rename or repurpose this property without a migration strategy: posts already stored on the server may depend on this key for restoring the quote after restart or history reload.
