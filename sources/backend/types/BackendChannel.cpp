/**
 * @file BackendChannel.cpp
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

#include <algorithm>

#include <QJsonObject>
#include <QDebug>
#include <QJsonArray>
#include "BackendChannel.h"
#include "BackendPoll.h"
#include "backend/Storage.h"
#include "log.h"

namespace Mattermost {

uint32_t BackendChannel::getChannelType (const QJsonObject& jsonObject)
{
	switch (jsonObject.value("type").toString()[0].unicode()) {
	case 'O':
		return publicChannel;
	case 'P':
		return privateChannel;
	case 'D':
		return directChannel;
	case 'G':
		return groupChannel;
	default:
		return unknown;
	}
}

QString BackendChannel::getChannelDescription () const
{
	return !header.isEmpty() ? header : purpose;
}

QString BackendChannel::getTeamAndChannelName ()
{
	switch (type) {
	case directChannel:
		return "(direct channel)/" + display_name;
	case groupChannel:
		return "(group channel)/" + display_name;
	default:
		return (team ? team->name : "(no team)") + "/" + display_name;
	}
}

void ChannelNewPosts::addChunk (ChannelNewPostsChunk&& chunk)
{
	if (chunk.postsToAdd.empty()) {
		return;
	}

	postsToAdd.emplace (postsToAdd.begin(), std::move (chunk));
}

BackendChannel::BackendChannel (Storage& storage, const QJsonObject& jsonObject)
:storage (storage)
{
	id = jsonObject.value("id").toString();
	create_at = jsonObject.value("create_at").toVariant().toULongLong();
	update_at = jsonObject.value("update_at").toVariant().toULongLong();
	delete_at = jsonObject.value("delete_at").toVariant().toULongLong();
	team = storage.getTeamById (jsonObject.value("team_id").toString());
	display_name = jsonObject.value("display_name").toString();
	name = jsonObject.value("name").toString();
	header = jsonObject.value("header").toString();
	purpose = jsonObject.value("purpose").toString();

	type = getChannelType (jsonObject);

	last_post_at = jsonObject.value("last_post_at").toVariant().toULongLong();

	total_msg_count = jsonObject.value("total_msg_count").toInt();
	has_total_msg_count_root = jsonObject.contains("total_msg_count_root");
	total_msg_count_root = has_total_msg_count_root
		? jsonObject.value("total_msg_count_root").toInt()
		: total_msg_count;
	extra_update_at = jsonObject.value("extra_update_at").toInt();
	creator = storage.getUserById (jsonObject.value("creator_id").toString());
	scheme_id = jsonObject.value("scheme_id").toVariant();
	props = jsonObject.value("props").toVariant();
	referenceCount = 1;
}

BackendChannel::~BackendChannel () = default;

BackendPost* BackendChannel::addPost (const QJsonObject& postObject)
{
	posts.emplace_back (postObject, storage);

	BackendPost* newPost = &posts.back ();
	postIdToPost[newPost->id] = newPost;

	QString rootId = postObject.value("root_id").toString();

	if (!rootId.isEmpty()) {
		rootIdAndPostList.push_back(QPair<QString,QString> (rootId, newPost->id));
		newPost->hidden = true;
		BackendPost* rootPost = findPostById(rootId);
		if (rootPost) {
			qDebug() << rootPost->id <<  rootPost->message;
			rootPost->has_thread = true;
			++rootPost->reply_count;
			rootPost->last_reply_at = std::max(rootPost->last_reply_at, newPost->create_at);
			emit onPostEdited(*rootPost);
		} else {
			missingRootPostIds.insert(rootId);
		}
	}

	if(missingRootPostIds.contains(newPost->id)) {
		newPost->has_thread = true;
		emit onPostEdited(*newPost);
		missingRootPostIds.remove(newPost->id);
	}

	return newPost;
}

void BackendChannel::addPost (const QJsonObject& postObject, std::list<BackendPost>::iterator position,
                              ChannelNewPostsChunk& currentChunk,
                              QVector<QPair<QString, QString>>& rootLinks, bool initialLoad)
{
	BackendPost* newPost = &*posts.emplace (position, postObject, storage);
	postIdToPost[newPost->id] = newPost;

	currentChunk.postsToAdd.emplace_front (newPost);

	QString rootId = postObject.value("root_id").toString();

	if (!rootId.isEmpty()) {
		rootLinks.push_back(QPair<QString,QString> (rootId, newPost->id));
		newPost->hidden = true;
		BackendPost* rootPost = findPostById(rootId);
		if (rootPost) {
			qDebug() << rootPost->id <<  rootPost->message;
			rootPost->has_thread = true;
			emit onPostEdited(*rootPost);
		} else {
			missingRootPostIds.insert(rootId);
		}
	}

	if(missingRootPostIds.contains(newPost->id)) {
		newPost->has_thread = true;
		emit onPostEdited(*newPost);
		missingRootPostIds.remove(newPost->id);
	}

	if (!initialLoad) {
		qDebug() << newPost->create_at << " " << newPost->id << " "
		         << newPost->getDisplayAuthorName() << " " << newPost->message;
	}
}

void BackendChannel::prependPosts (const QJsonArray& orderArray, const QJsonObject& postsObject)
{
	ChannelNewPosts allNewPosts;
	ChannelNewPostsChunk currentNewPostsChunk;

	bool initialLoad = true;

	for (const auto& newPostEl: orderArray) {
		QString newPostId = newPostEl.toString();
		addPost (postsObject.find (newPostId).value().toObject(), posts.begin (),
		         currentNewPostsChunk, rootIdAndPostList, initialLoad);
	}

	if (!currentNewPostsChunk.postsToAdd.empty()) {
		allNewPosts.addChunk (std::move (currentNewPostsChunk));
	}

	emit onNewPosts (allNewPosts);
}

void BackendChannel::addPosts (const QJsonArray& orderArray, const QJsonObject& postsObject)
{
	ChannelNewPosts allNewPosts;
	ChannelNewPostsChunk currentNewPostsChunk;

	std::list<BackendPost>::reverse_iterator currentLocalPost = posts.rbegin();

#if defined(_MSC_VER)
#pragma message("warning: Handle case of deleted post, that is not deleted locally")
#else
#warning "Handle case of deleted post, that is not deleted locally"
#endif

	bool initialLoad = (posts.empty());
	bool lastPostWasSkipped = false;

	for (const auto& newPostEl: orderArray) {
		while (currentLocalPost != posts.rend() && currentLocalPost->isDeleted) {
			++currentLocalPost;
		}

		QString newPostId = newPostEl.toString();

		if (currentLocalPost == posts.rend()) {
			addPost (postsObject.find (newPostId).value().toObject(), posts.begin (),
			         currentNewPostsChunk, rootIdAndPostList, initialLoad);
			++currentLocalPost;
			continue;
		}

		if (currentLocalPost->id == newPostId) {
			++currentLocalPost;

			if (lastPostWasSkipped) {
				currentNewPostsChunk.previousPostId = newPostId;
				allNewPosts.addChunk (std::move (currentNewPostsChunk));
				lastPostWasSkipped = false;
			}
			continue;
		}

		qDebug () << "Add after currentLocalPost";
		addPost (postsObject.find (newPostId).value().toObject(), currentLocalPost.base(),
		         currentNewPostsChunk, rootIdAndPostList, initialLoad);
		++currentLocalPost;
		lastPostWasSkipped = true;
	}

	if (!currentNewPostsChunk.postsToAdd.empty()) {
		if (currentLocalPost != posts.rend()) {
			currentNewPostsChunk.previousPostId = currentLocalPost->id;
		}
		allNewPosts.addChunk (std::move (currentNewPostsChunk));
	}

	emit onNewPosts (allNewPosts);
}

void BackendChannel::mergePostContext(const QJsonArray& orderArray, const QJsonObject& postsObject)
{
	ChannelNewPosts allNewPosts;

	// Mattermost PostList order is newest -> oldest. Process it in the opposite
	// direction so every newly inserted row can reference the already-present
	// previous row in its own visible timeline. That lets an active ChatArea
	// insert arbitrary context without rebuilding the whole list.
	for (auto orderIt = orderArray.crbegin(); orderIt != orderArray.crend(); ++orderIt) {
		const QString newPostId = orderIt->toString();
		if (newPostId.isEmpty() || postIdToPost.contains(newPostId)) {
			continue;
		}

		const auto postIt = postsObject.constFind(newPostId);
		if (postIt == postsObject.constEnd() || !postIt->isObject()) {
			continue;
		}

		const QJsonObject postObject = postIt->toObject();
		const uint64_t createAt = postObject.value(QStringLiteral("create_at"))
			.toVariant().toULongLong();
		const QString rootId = postObject.value(QStringLiteral("root_id")).toString();

		auto position = posts.begin();
		for (; position != posts.end(); ++position) {
			if (position->create_at > createAt
				|| (position->create_at == createAt && position->id > newPostId)) {
				break;
			}
		}

		QString previousPostId;
		auto previous = position;
		while (previous != posts.begin()) {
			--previous;
			const bool sameTimeline = rootId.isEmpty()
				? (previous->root_id.isEmpty() && !previous->hidden)
				: (previous->id == rootId || previous->root_id == rootId);
			if (sameTimeline) {
				previousPostId = previous->id;
				break;
			}
		}

		ChannelNewPostsChunk chunk;
		chunk.previousPostId = previousPostId;
		addPost(postObject, position, chunk, rootIdAndPostList, false);
		if (!chunk.postsToAdd.empty()) {
			// We are already iterating oldest -> newest, so do not use addChunk(),
			// which intentionally reverses chunks produced by addPosts().
			allNewPosts.postsToAdd.emplace_back(std::move(chunk));
		}
	}

	if (!allNewPosts.postsToAdd.empty()) {
		emit onNewPosts(allNewPosts);
	}
}

void BackendChannel::addPinnedPosts (const QJsonArray& orderArray, const QJsonObject& postsObject)
{
	pinnedPosts.clear ();

	for (const auto& newPostEl: orderArray) {
		QString newPostId = newPostEl.toString();
		pinnedPosts.emplace_back (postsObject.find (newPostId).value().toObject(), storage);
	}

	// Consumers also need the empty response so a previously visible pinned-post
	// button/panel can be cleared after the last pin is removed.
	emit onPinnedPostsReceived ();
}

void BackendChannel::editPost (BackendPost& newPost)
{
	BackendPost* existingPost = findPostById (newPost.id);

	if (!existingPost) {
		LOG_DEBUG ("BackendChannel::editPost: post with ID " << newPost.id << " not found");
		return;
	}

	existingPost->updatePostEdits (newPost);
	emit onPostEdited (*existingPost);
}

void BackendChannel::addPostReaction (QString postId, QString userId, QString emojiName)
{
	BackendPost* existingPost = findPostById (postId);

	if (!existingPost) {
		LOG_DEBUG ("BackendChannel::addPostReaction: post with ID " << postId << " not found");
		return;
	}

	existingPost->addReaction (storage.getUserDisplayNameByUserId (userId, true), emojiName);
	emit onPostReactionUpdated (*existingPost);
}

void BackendChannel::removePostReaction (QString postId, QString userId, QString emojiName)
{
	BackendPost* existingPost = findPostById (postId);

	if (!existingPost) {
		LOG_DEBUG ("BackendChannel::addPostReaction: post with ID " << postId << " not found");
		return;
	}

	existingPost->removeReaction (storage.getUserDisplayNameByUserId (userId, true), emojiName);
	emit onPostReactionUpdated (*existingPost);
}

QSet<const BackendUser*> BackendChannel::getAllMembers () const
{
	QSet<const BackendUser*> ret;

	for (auto& member: members) {
		if (member.user) {
			ret.insert (member.user);
		}
	}

	return ret;
}

void BackendChannel::addMember (const Storage& channelStorage, const QJsonObject& jsonObject)
{
	BackendChannelMember member (channelStorage, jsonObject);
	if (member.userId.isEmpty()) {
		LOG_DEBUG ("Channel " << display_name << " addMember: empty user id");
		return;
	}
	members.insert (member.userId, std::move (member));
}

BackendPost* BackendChannel::findPostById (QString postID)
{
	auto it = postIdToPost.find (postID);

	if (it == postIdToPost.end()) {
		return nullptr;
	}

	return it.value();
}

} /* namespace Mattermost */
