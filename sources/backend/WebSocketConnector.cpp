/**
 * @file WebSocketConnector.cpp
 * @brief
 * @author Lyubomir Filipov
 * @date Dec 28, 2021
 *
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
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

#include "WebSocketConnector.h"

#include <iostream>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRandomGenerator>
#include <QUrlQuery>

#include "backend/WebSocketEventHandler.h"
#include "log.h"

namespace Mattermost {

namespace {

constexpr int HeartbeatIntervalMs = 30000;
constexpr int MinReconnectDelayMs = 3000;
constexpr int MaxReconnectDelayMs = 300000;
constexpr int ReconnectJitterMs = 2000;
constexpr int BackoffThreshold = 7;

template<typename T>
void handler (WebSocketConnector& conn, const QJsonObject& data, const QJsonObject& broadcast)
{
	conn.eventHandler.handleEvent (T (data, broadcast));
}

const QMap<QString, void(*)(WebSocketConnector&, const QJsonObject&, const QJsonObject&)> eventHandlers {
	{"hello", [] (WebSocketConnector&, const QJsonObject&, const QJsonObject&) {
		std::cout << "Hello" << std::endl;
	}},
	{"channel_viewed",		handler<ChannelViewedEvent>},
	{"posted", 				handler<PostEvent>},
	{"post_edited", 		handler<PostEditedEvent>},
	{"post_deleted",		handler<PostDeletedEvent>},
	{"reaction_added",		handler<PostReactionAddedEvent>},
	{"reaction_removed",	handler<PostReactionRemovedEvent>},
	{"typing",				handler<TypingEvent>},
	{"status_change", 		handler<StatusChangeEvent>},
	{"direct_added", 		handler<NewDirectChannelEvent>},		//new direct channel created
	{"new_user",			handler<NewUserEvent>},				//user added to the server
	{"user_updated",		handler<UserUpdatedEvent>},			//user data updated
	{"user_added",			handler<UserAddedToChannelEvent>},		//user added to channel
	{"added_to_team",		handler<UserAddedToTeamEvent>},			//user added to team
	{"leave_team",			handler<UserLeaveTeamEvent>},			//a user has left a team
	{"user_removed",		handler<UserRemovedFromChannelEvent>},	//a user (the logged-in user, or someone else) was removed from a channel
	{"channel_created",		handler<ChannelCreatedEvent>},			//a new channel was created
	{"channel_updated",		handler<ChannelUpdatedEvent>},			//a channel was updated
	{"open_dialog",			handler<OpenDialogEvent>},				//a server-side dialog
};

bool printEvent (const QString& name)
{
	if (	name == "channel_viewed" 	||
			name == "channel_updated" 	||
			name == "reaction_added" 	||
			name == "status_change" 	||
			name == "posted" 			||
			//name == "typing" 			||
			name == "reaction_removed"	||
			name == "user_removed"		||
			name == "user_updated"		||
			name == "leave_team"
	) {
		return false;
	}

	return true;
}

} // namespace

WebSocketConnector::WebSocketConnector (WebSocketEventHandler& eventHandler)
:eventHandler (eventHandler)
,responseSequence (1)
,serverSequence (0)
,pendingPingSequence (0)
,reconnectAttempt (0)
,waitingForPong (false)
,hasReconnect (false)
,helloReceived (false)
,suppressReconnect (false)
{
	auto errorHandler = [this] (QAbstractSocket::SocketError error) {
		LOG_DEBUG ("WebSocket error " << error << " " << webSocket.errorString());
		if (!suppressReconnect && webSocket.state() == QAbstractSocket::UnconnectedState) {
			scheduleReconnect ();
		}
	};
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
	connect (&webSocket, &QWebSocket::errorOccurred, this, errorHandler);
#else
	connect (&webSocket, qOverload<QAbstractSocket::SocketError>(&QWebSocket::error), this, errorHandler);
#endif

	connect (&webSocket, &QWebSocket::connected, this, [this] {
		LOG_DEBUG ("WebSocket connected");
		reconnectTimer.stop ();
		responseSequence = 1;
		helloReceived = false;
		doHandshake ();
		startHeartbeat ();
	});

	connect (&webSocket, &QWebSocket::disconnected, this, [this] {
		stopHeartbeat ();
		responseSequence = 1;

		const bool reconnectSuppressed = suppressReconnect;
		suppressReconnect = false;

		LOG_DEBUG ("WebSocket disconnected. Code: " << webSocket.closeCode() << " " << webSocket.closeReason());
		emit onDisconnect ();
		helloReceived = false;

		if (!reconnectSuppressed && !token.isEmpty()) {
			scheduleReconnect ();
		}
	});

	connect (&webSocket, &QWebSocket::textMessageReceived,
			 this, &WebSocketConnector::onNewPacket);

	heartbeatTimer.setInterval (HeartbeatIntervalMs);
	connect (&heartbeatTimer, &QTimer::timeout, this, &WebSocketConnector::sendPing);

	reconnectTimer.setSingleShot (true);
	connect (&reconnectTimer, &QTimer::timeout, this, [this] {
		if (token.isEmpty() || suppressReconnect) {
			return;
		}
		if (webSocket.state() != QAbstractSocket::UnconnectedState) {
			return;
		}

		LOG_DEBUG ("WebSocket Reconnecting (connection_id=" << connectionId
				   << ", sequence_number=" << serverSequence << ")");
		openSocket ();
	});
}

WebSocketConnector::~WebSocketConnector () = default;

void WebSocketConnector::open (const QString& urlString, const QString& authToken)
{
	endpointUrl = QUrl (urlString + "websocket");
	endpointUrl.setScheme ("wss");

	token = authToken;
	connectionId.clear ();
	responseSequence = 1;
	serverSequence = 0;
	pendingPingSequence = 0;
	reconnectAttempt = 0;
	waitingForPong = false;
	hasReconnect = false;
	helloReceived = false;
	suppressReconnect = false;
	reconnectTimer.stop ();
	stopHeartbeat ();

	openSocket ();
}

void WebSocketConnector::close ()
{
	token.clear ();
	reset ();
}

void WebSocketConnector::scheduleReconnect ()
{
	stopHeartbeat ();

	if (token.isEmpty() || suppressReconnect || reconnectTimer.isActive()) {
		return;
	}
	if (webSocket.state() != QAbstractSocket::UnconnectedState) {
		return;
	}

	hasReconnect = true;
	++reconnectAttempt;

	int delay = MinReconnectDelayMs;
	if (reconnectAttempt > BackoffThreshold) {
		const qint64 scaledDelay = static_cast<qint64>(MinReconnectDelayMs)
			* reconnectAttempt * reconnectAttempt;
		delay = static_cast<int>(qMin<qint64>(scaledDelay, MaxReconnectDelayMs));
	}
	delay += static_cast<int>(QRandomGenerator::global()->bounded(static_cast<quint32>(ReconnectJitterMs)));

	LOG_DEBUG ("WebSocket reconnect scheduled in " << delay << " ms");
	reconnectTimer.start (delay);
}

QUrl WebSocketConnector::socketUrl () const
{
	QUrl url (endpointUrl);
	QUrlQuery query (url);
	query.removeAllQueryItems (QStringLiteral("connection_id"));
	query.removeAllQueryItems (QStringLiteral("sequence_number"));
	query.addQueryItem (QStringLiteral("connection_id"), connectionId);
	query.addQueryItem (QStringLiteral("sequence_number"), QString::number(serverSequence));
	url.setQuery (query);
	return url;
}

void WebSocketConnector::openSocket ()
{
	if (token.isEmpty() || endpointUrl.isEmpty()) {
		return;
	}

	const QUrl url = socketUrl ();
	LOG_DEBUG ("WebSocket opening " << url.toString(QUrl::RemovePassword));
	webSocket.open (url);
}

void WebSocketConnector::doHandshake ()
{
	QJsonObject jsonData {
		{"token", token},
	};

	const int sequence = responseSequence++;
	QJsonDocument json (QJsonObject {
		{"seq", sequence},
		{"action", "authentication_challenge"},
		{"data", jsonData},
	});

	webSocket.sendTextMessage (json.toJson(QJsonDocument::Compact));
}

void WebSocketConnector::startHeartbeat ()
{
	stopHeartbeat ();
	sendPing ();
	heartbeatTimer.start ();
}

void WebSocketConnector::stopHeartbeat ()
{
	heartbeatTimer.stop ();
	waitingForPong = false;
	pendingPingSequence = 0;
}

void WebSocketConnector::sendPing ()
{
	if (webSocket.state() != QAbstractSocket::ConnectedState) {
		return;
	}

	// Mattermost's web client uses an application-level websocket action named
	// "ping". It does not rely on RFC6455 control-frame ping/pong for its
	// liveness check. The response is a normal {status, seq_reply} packet.
	if (waitingForPong) {
		LOG_DEBUG ("Mattermost WebSocket ping received no response within "
				   << HeartbeatIntervalMs << " ms. Reconnecting");
		webSocket.abort ();
		return;
	}

	const int sequence = responseSequence++;
	pendingPingSequence = sequence;
	waitingForPong = true;

	QJsonDocument json (QJsonObject {
		{"seq", sequence},
		{"action", "ping"},
	});
	webSocket.sendTextMessage (json.toJson(QJsonDocument::Compact));
}

void WebSocketConnector::reset ()
{
	reconnectTimer.stop ();
	stopHeartbeat ();
	connectionId.clear ();
	responseSequence = 1;
	serverSequence = 0;
	reconnectAttempt = 0;
	hasReconnect = false;
	helloReceived = false;

	if (webSocket.state() != QAbstractSocket::UnconnectedState) {
		suppressReconnect = true;
		webSocket.close (QWebSocketProtocol::CloseCodeNormal, QStringLiteral("Client Close"));
	} else {
		suppressReconnect = false;
	}
}

void WebSocketConnector::onNewPacket (const QString& string)
{
	QJsonParseError parseError;
	const QJsonDocument doc = QJsonDocument::fromJson (string.toUtf8(), &parseError);
	if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
		LOG_DEBUG ("Invalid WebSocket JSON: " << parseError.errorString());
		return;
	}

	const QJsonObject jsonObject = doc.object ();

	// Replies to websocket actions use the request's sequence number and are
	// separate from the server event stream sequence.
	const QJsonValue seqReply = jsonObject.value (QStringLiteral("seq_reply"));
	if (!seqReply.isUndefined()) {
		const int replySequence = seqReply.toInt ();
		if (waitingForPong && replySequence == pendingPingSequence) {
			waitingForPong = false;
			pendingPingSequence = 0;
		}

		if (jsonObject.contains(QStringLiteral("error"))) {
			LOG_DEBUG ("WebSocket action failed: " << doc.toJson(QJsonDocument::Compact));
		}
		return;
	}

	const QString eventName = jsonObject.value (QStringLiteral("event")).toString ();

	// A hello packet identifies the reliable websocket stream. On reconnect the
	// server reuses connection_id and resumes from sequence_number when its
	// backlog is still available. A different id means replay was not possible;
	// reset the expected sequence and let Backend's reconnect sync fill the gap.
	if (eventName == QStringLiteral("hello")) {
		const QString newConnectionId = jsonObject.value(QStringLiteral("data"))
			.toObject().value(QStringLiteral("connection_id")).toString();
		if (!connectionId.isEmpty() && !newConnectionId.isEmpty()
			&& connectionId != newConnectionId) {
			LOG_DEBUG ("Mattermost started a new WebSocket stream (old connection_id="
					   << connectionId << ", new connection_id=" << newConnectionId
					   << "). Falling back to HTTP resync");
			serverSequence = 0;
		}
		if (!newConnectionId.isEmpty()) {
			connectionId = newConnectionId;
		}
	}

	const QJsonValue eventSequenceValue = jsonObject.value (QStringLiteral("seq"));
	if (!eventSequenceValue.isUndefined()) {
		const int eventSequence = eventSequenceValue.toInt (-1);
		if (eventSequence != serverSequence) {
			LOG_DEBUG ("Missed WebSocket event: received seq=" << eventSequence
					   << ", expected seq=" << serverSequence
					   << ". Reconnecting with reliable sequence recovery");
			webSocket.abort ();
			return;
		}
		serverSequence = eventSequence + 1;
	}

	if (eventName == QStringLiteral("hello") && !helloReceived) {
		helloReceived = true;
		reconnectAttempt = 0;
		const bool reconnected = hasReconnect;
		hasReconnect = false;
		emit onConnect (reconnected);
	}

	auto it = eventHandlers.find (eventName);
	if (it == eventHandlers.end()) {
		//sometimes MM instance keeps spamming custom_profile_attributes_values_updated
		//custom profile attributes will not be supported in the near future
		if (eventName != QStringLiteral("custom_profile_attributes_values_updated")) {
			LOG_DEBUG ("Unhandled WebSocket event '" << eventName << "'\n");
			const QString jsonString = doc.toJson (QJsonDocument::Indented);
			std::cout << jsonString.toStdString ();
			qDebug() << "========" << '\n';
		}
		return;
	}

	if (printEvent (it.key())) {
		qDebug() << "========" << '\n';
		const QString jsonString = doc.toJson (QJsonDocument::Indented);
		std::cout << jsonString.toStdString ();
	}

	it.value() (*this,
				jsonObject.value (QStringLiteral("data")).toObject(),
				jsonObject.value (QStringLiteral("broadcast")).toObject());
}

} /* namespace Mattermost */
