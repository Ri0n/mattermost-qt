/**
 * @file BackendChannel.h
 * @brief
 * @author Lyubomir Filipov
 * @date Dec 1, 2021
 *
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Mattermost-QT is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Mattermost-QT. if not, see https://www.gnu.org/licenses/.
 */

#pragma once

#include <QVariant>
#include "BackendPost.h"
#include "BackendChannelMember.h"
#include "BackendChannelProperties.h"
#include "fwd.h"

class QJsonObject;

namespace Mattermost {

class Storage;

/**
 * A sequence of new posts
 */
struct ChannelNewPostsChunk {
	QString 									previousPostId;
	std::list<BackendPost*>						postsToAdd;
};

/**
 * Collections of missing posts for a channel.
 * Missing posts can happen if a webSocket disconnect occurs and new posts arrive before reconnect
 */
struct ChannelNewPosts {

	void addChunk (ChannelNewPostsChunk&& chunk);

	std::vector<ChannelNewPostsChunk>		postsToAdd;
};


class BackendChannel: public QObject {
	Q_OBJECT
public:
	enum type {
		unknown,
		publicChannel,
		privateChannel,
		directChannel,		//!< Direct channel, has only 2 users
		groupChannel,		//!< Group channel. Like direct, but has more than 2 users. Does not belong to any team
	};
	BackendChannel (Storage& storage, const QJsonObject& jsonObject);
	virtual ~BackendChannel ();
public:
	static uint32_t getChannelType (const QJsonObject& jsonObject);

	QString getChannelDescription () const;
	QString getTeamAndChannelName ();
	/** Rebuild a group-DM title from loaded member profiles. */
	bool refreshGroupDisplayName();

	QSet<const BackendUser*> getAllMembers () const;
	void addMember (const Storage& storage, const QJsonObject& jsonObject);

	BackendPost* addPost (const QJsonObject& postObject);

	/** Whether removing this resident body would leave no raw root dependency dangling. */
	bool canEvictPostBody(const QString& postId) const;
	/** Remove one resident body while preserving all external logical source identities. */
	bool evictPostBody(const QString& postId);
	/** Re-announce an insertion hidden by a selective quiet-ingest signal blocker. */
	void notifyPostBodyAvailable(const QString& postId);

	void prependPosts (const QJsonArray& orderArray, const QJsonObject& postsObject);
	void addPosts (const QJsonArray& orderArray, const QJsonObject& postsObject);
	// Merge an arbitrary server context window by identity/timestamp. Unlike
	// addPosts(), this does not assume that the response is the newest channel
	// edge and therefore does not infer deletions from gaps. It is used by
	// pinned/Attention navigation around a specific post.
	void mergePostContext (const QJsonArray& orderArray, const QJsonObject& postsObject);
	void addPinnedPosts (const QJsonArray& orderArray, const QJsonObject& postsObject);
	void editPost (BackendPost& newPost);
	void deletePost (QString postId);
	void addPostReaction (QString postId, QString userId, QString emojiName);
	void removePostReaction (QString postId, QString userId, QString emojiName);
signals:

	/**
	 * Called when a channel is being viewed, either from this or from another Mattermost instance.
	 * Same notification as Backend::onChannelViewed()
	 */
	void onViewed ();

	/**
	 * Called when a channel is being updated. For example, changing the channel title or image
	 */
	void onUpdated ();

	/**
	 * Called when new posts are received.
	 * Either after init or after WebSocket disconnect.
	 *
	 * @param posts Collection of the missing posts only
	 */
	void onNewPosts (const ChannelNewPosts& collection);

	/**
	 * Called when pinned posts are received.
	 * Either after init or after WebSocket disconnect.
	 */
	void onPinnedPostsReceived ();

	/**
	 * Called when a new post has arrived in the channel
	 * Same notification as Backend::onNewPost()
	 * @param post post
	 */
	void onNewPost (BackendPost& post);

	/**
	 * Called when a post is being edited
	 */
	void onPostEdited (BackendPost& post);

	/** Called when a post reaction is being updated. */
	void onPostReactionUpdated (BackendPost& post);

	/** Called when a post is deleted. */
	void onPostDeleted (const QString& postId);

	/** Resident body appeared/disappeared while its semantic timeline ID may remain mapped. */
	void onPostBodyAvailabilityChanged(const QString& postId, bool available);

	void onUserTyping (const BackendUser& user);
	void onMissedPosts ();
	void onLeave ();
	void onUserAdded (const BackendUser& user);
	void onUserRemoved (const BackendUser& user);
private:
	void addPost (const QJsonObject& postObject, std::list<BackendPost>::iterator position, ChannelNewPostsChunk& currentChunk, QVector<QPair<QString, QString>>& rootLinks, bool initialLoad);
	BackendPost* findPostById (QString postID);
public:
	const Storage&					storage;
    QString							id;
    uint64_t						create_at;
    uint64_t						update_at;
    uint64_t						delete_at;
    BackendTeam*					team;
    QString							display_name;
    QString							name;
    QString							header;
    QString							purpose;
    int								type;
    uint64_t						last_post_at;
    // Newest root-post timestamp. Unlike last_post_at, thread replies do not
    // advance this value, so it is the correct freshness marker for the main
    // channel root timeline/cache suffix. Older servers may omit the field;
    // the constructor then falls back to last_post_at.
    uint64_t						last_root_post_at;
    int								total_msg_count;
    int								total_msg_count_root;
    bool								has_total_msg_count_root;
    int								extra_update_at;
    const BackendUser*				creator;
    QMap<QString, BackendChannelMember> 		members;
    QVariant						scheme_id;
    QVariant						props;
    uint32_t						referenceCount;

    QMap<QString, BackendPost*>		postIdToPost;
    QSet<QString>			missingRootPostIds;
    std::list<BackendPost>			posts;
    std::list<BackendPost>			pinnedPosts;

    QVector<QPair<QString, QString>> rootIdAndPostList;
};

} /* namespace Mattermost */
