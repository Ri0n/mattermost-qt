/**
 * @file EmojiInfo.cpp
 * @brief Contains functions for getting emoji by ID and adding custom emojis
 * @author Lyubomir Filipov
 * @date Dec 30, 2022
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

#include "EmojiInfo.h"

#include <QDebug>
#include <QDir>
#include <QMap>
#include <QSet>
#include <QUrl>

#include "EmojiRegistryNotifier.h"

namespace Mattermost {

extern uint32_t lastCategorySeq[EmojiCategory::COUNT];
extern QVector<Emoji> emojiVecNoSkinVariadic[EmojiCategory::COUNT];
extern QVector<SkinVariadicEmoji> emojiVecSkinVariadic;
extern QMap<QString, EmojiSeq> emojiMap;
extern uint32_t nextEmojiSeq;

namespace {

QSet<QString> customEmojiPaths;

bool isValidCustomEmojiName(const QString& name)
{
    if (name.isEmpty()) {
        return false;
    }

    for (const QChar character : name) {
        const ushort value = character.unicode();
        const bool asciiLetter = (value >= 'A' && value <= 'Z')
            || (value >= 'a' && value <= 'z');
        const bool asciiDigit = value >= '0' && value <= '9';
        if (!asciiLetter && !asciiDigit
            && character != QLatin1Char('_')
            && character != QLatin1Char('-')
            && character != QLatin1Char('+')) {
            return false;
        }
    }
    return true;
}

void requestCustomEmoji(const QString& name)
{
    if (isValidCustomEmojiName(name)) {
        emit EmojiRegistryNotifier::instance().customEmojiRequested(name);
    }
}

QString normalizedCustomEmojiPath(QString path)
{
    if (path.isEmpty()) {
        return {};
    }

    const QUrl url(path);
    if (url.isLocalFile()) {
        path = url.toLocalFile();
    }
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}

} // namespace

/**
 * Search for a skin tone string in the emoji name. Remove it, when performing lookup,
 * because emoji names are stored without the skin tone. All skin tone emojis are in an array and
 * the lookup is performed without the skin tone name.
 *
 * Also, the skin tone lookup order is changed, so that searching for 'light' will not find 'medium_light'
 */
static QString skinTonelookup[] {"", "medium_light", "medium_dark", "light", "medium", "dark"};
static const uint16_t skinToneLookupMap[] {
		EmojiSkinTone::none,
		EmojiSkinTone::mediumLight,
		EmojiSkinTone::mediumDark,
		EmojiSkinTone::light,
		EmojiSkinTone::medium,
		EmojiSkinTone::dark,
};

EmojiID EmojiInfo::findByName (const QString& emojiName)
{
	for (uint16_t i = 1; i < EmojiSkinTone::COUNT; ++i) {

		QString lookup ("_" + skinTonelookup[i] + "_skin_tone");

		int found = emojiName.indexOf (lookup);
		if (found != -1) {
			QString emojiNameReplaced (emojiName);
			emojiNameReplaced.remove (lookup);

			auto it = emojiMap.find (emojiNameReplaced);

			if (it == emojiMap.end ()) {
                requestCustomEmoji(emojiName);
				return {0,0};
			}

			return {skinToneLookupMap[i], it.value()};
		}
	}

	auto it = emojiMap.find (emojiName);

	if (it == emojiMap.end ()) {
        requestCustomEmoji(emojiName);
		return {0,0};
	}

	return {0, it.value()};
}

static int getEmojiCategory (uint16_t emojiSeq)
{
	for (int i = 0; i < EmojiCategory::COUNT; ++i) {
		if (emojiSeq <= lastCategorySeq[i]) {
			return i;
		}
	}

	return EmojiCategory::COUNT;
}

Emoji EmojiInfo::getEmoji (const EmojiID& emojiID)
{
	if (!emojiID) {
		qDebug () << "No emoji with seq " << emojiID.seq << " found";
		return Emoji {"",""};
	}

	int category = getEmojiCategory (emojiID.seq);

	if (category < EmojiCategory::COUNT) {
		int emojiIndex = emojiID.seq - 1 - (lastCategorySeq[category] - emojiVecNoSkinVariadic[category].size());
		return emojiVecNoSkinVariadic[category][emojiIndex];
	}

	if (emojiID.seq < SKINVARIADIC_START_INDEX) {
		qDebug () << "No emoji with seq " << emojiID.seq << " found";
		return Emoji {"",""};
	}

	int emojiIndex = emojiID.seq - SKINVARIADIC_START_INDEX;

	if (emojiIndex < emojiVecSkinVariadic.size()) {
		SkinVariadicEmoji variadicEmoji = emojiVecSkinVariadic[emojiIndex];

		QString emojiName (variadicEmoji.name);

		if (emojiID.skinTone) {
			emojiName += " (skin tone: " + skinTonelookup[emojiID.skinTone] + ")";
		}

		return Emoji {emojiName, variadicEmoji.unicodeString[emojiID.skinTone]};
	}

	qDebug () << "No emoji with seq " << emojiID.seq << " found";
	return {"",""};
}

QVector<Emoji> EmojiInfo::getAllEmojis (uint32_t category, uint32_t skinTone)
{
	QVector<Emoji> ret (emojiVecNoSkinVariadic[category]);

	if (category == EmojiCategory::people) {
		for (auto& it: emojiVecSkinVariadic) {
			ret.push_back (Emoji {it.name, it.unicodeString[skinTone]});
		}
	}

	return ret;
}

void EmojiInfo::addCustomEmoji (const QString& emojiName, const QString& emojiPath)
{
    const QString normalizedPath = normalizedCustomEmojiPath(emojiPath);
    const auto existing = emojiMap.constFind(emojiName);
    if (existing != emojiMap.cend()
        && getEmojiCategory(existing.value()) == EmojiCategory::custom) {
        if (!normalizedPath.isEmpty()) {
            customEmojiPaths.insert(normalizedPath);
        }
        return;
    }

    // width/height are deliberately a non-presentation sentinel. Qt 6's
    // MessageFormatter uses explicit image dimensions to distinguish generated
    // custom emoji from user Markdown images while serializing the document;
    // every actual renderer replaces this 1px box through EmojiPresentation.
	emojiVecNoSkinVariadic[EmojiCategory::custom].push_back (
        Emoji {emojiName,
               QStringLiteral(" <img src=\"%1\" width=1 height=1> ")
                   .arg(emojiPath.toHtmlEscaped())});
	emojiMap[emojiName] = nextEmojiSeq;
	++nextEmojiSeq;
	++lastCategorySeq[EmojiCategory::custom];
    if (!normalizedPath.isEmpty()) {
        customEmojiPaths.insert(normalizedPath);
    }

    emit EmojiRegistryNotifier::instance().customEmojiAdded(emojiName);
}

bool EmojiInfo::isCustomEmojiPath(const QString& emojiPath)
{
    const QString normalizedPath = normalizedCustomEmojiPath(emojiPath);
    return !normalizedPath.isEmpty() && customEmojiPaths.contains(normalizedPath);
}

} /* namespace Mattermost */