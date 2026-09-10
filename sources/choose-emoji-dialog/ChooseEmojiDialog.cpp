/**
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

#include "ChooseEmojiDialog.h"

#include <QComboBox>
#include <QDebug>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSettings>

#include "EmojiDialogSupport.h"
#include "backend/emoji/EmojiInfo.h"
#include "ui_ChooseEmojiDialog.h"

namespace Mattermost {

static constexpr int itemsPerRow = 30;
static constexpr int maxSearchResults = 180;
static constexpr uint32_t searchCategory = EmojiCategory::COUNT + 1;

/**
 * Index of the emoji to be shown before the tab name for the current category.
 * The index is set to specify the emoji which most clearly describes the meaning of the category.
 */
static const uint32_t indexForCategoryTab[EmojiCategory::COUNT] = {
    0,   // smileys-emotion
    98,  // people-body
    0,   // component
    1,   // animals-nature
    2,   // food-drink
    0,   // travel-places
    30,  // activities
    1,   // objects
    120, // symbols
    0,   // flags
    0,   // custom
};

ChooseEmojiDialog::ChooseEmojiDialog(QWidget* parent)
    : QDialog(parent)
    , ui(new Ui::ChooseEmojiDialog)
{
    ui->setupUi(this);
    connect(ui->searchEdit, &QLineEdit::textChanged,
            this, &ChooseEmojiDialog::updateSearchResults);
}

ChooseEmojiDialog::~ChooseEmojiDialog()
{
    delete ui;
}

Emoji ChooseEmojiDialog::getSelectedEmoji()
{
    return selectedEmoji;
}

void ChooseEmojiDialog::show()
{
    createEmojiTabs();
    ui->searchEdit->clear();
    QDialog::show();
    ui->searchEdit->setFocus(Qt::ShortcutFocusReason);
}

QGridLayout* ChooseEmojiDialog::createTab(uint32_t categoryIdx, int tabIndex)
{
    QWidget* tab = new QWidget;
    tab->setObjectName(QStringLiteral("tab") + QString::number(categoryIdx));
    QGridLayout* gridLayout = new QGridLayout(tab);
    gridLayout->setSpacing(0);
    gridLayout->setContentsMargins(0, 0, 0, 0);

    if (tabIndex >= ui->tabWidget->count()) {
        ui->tabWidget->addTab(tab, QString());
    } else {
        QWidget* oldTab = ui->tabWidget->widget(tabIndex);
        ui->tabWidget->removeTab(tabIndex);
        ui->tabWidget->insertTab(tabIndex, tab, QString());
        if (oldTab) {
            oldTab->deleteLater();
        }
    }
    return gridLayout;
}

void ChooseEmojiDialog::restoreEmojiFavorites()
{
    QSettings settings;
    QByteArray favoritesArray = settings.value("emoji_favorites").value<QByteArray>();

#if QT_VERSION >= QT_VERSION_CHECK(5,14,0)
    QVector<EmojiID> favoriteEmojisVec((EmojiID*)favoritesArray.begin(),
                                       (EmojiID*)favoritesArray.end());
#else
    QVector<EmojiID> favoriteEmojisVec;
    size_t size = favoritesArray.size() / sizeof(EmojiID);
    favoriteEmojisVec.reserve(size);
    std::copy((EmojiID*)favoritesArray.begin(),
              (EmojiID*)favoritesArray.end(),
              std::back_inserter(favoriteEmojisVec));
#endif

    if (favoriteEmojisVec.isEmpty()) {
        QString favoriteEmojiNames[] = {
            "+1",
            "pray",
            "brain",
            "smiley",
            "rolling_on_the_floor_laughing",
            "sunglasses",
            "mask",
            "face_vomiting",
            "yawning_face",
            "cherries",
            "pizza",
            "warning",
            "radioactive_sign",
            "white_check_mark",
            "heavy_check_mark",
        };

        for (auto& it : favoriteEmojiNames) {
            EmojiID id = EmojiInfo::findByName(it);
            favorites.insert(id, EmojiInfo::getEmoji(id));
        }

        saveEmojiFavorites();
        return;
    }

    for (auto& it : favoriteEmojisVec) {
        favorites.insert(it, EmojiInfo::getEmoji(it));
    }
}

void ChooseEmojiDialog::saveEmojiFavorites()
{
    QSettings settings;

    QVector<EmojiID> vec = favorites.keys().toVector();
    QByteArray favoritesArray((const char*)vec.data(), vec.size() * sizeof(vec[0]));
    settings.setValue("emoji_favorites", QVariant::fromValue(favoritesArray));
    qDebug() << "Save Emoji Favorites";
}

void ChooseEmojiDialog::updateFavoritesTab()
{
    createTabForCategory(EmojiCategory::favorites, 0, "Favorites",
                         favorites.values().toVector());
}

void ChooseEmojiDialog::createEmojiTabs()
{
    // Tabs already created.
    if (ui->tabWidget->count() >= 1) {
        return;
    }

    restoreEmojiFavorites();
    searchableEmojis.clear();
    uint32_t tabIndex = 0;

    createTabForCategory(EmojiCategory::favorites, tabIndex,
                         "Favorites", favorites.values().toVector());
    ++tabIndex;

    for (uint32_t categoryIdx = 0; categoryIdx < EmojiCategory::COUNT; ++categoryIdx) {
        // The component category contains only skin-tone images and is not useful alone.
        if (categoryIdx == EmojiCategory::component) {
            continue;
        }

        const QVector<Emoji> emojis = EmojiInfo::getAllEmojis(categoryIdx, 0);
        searchableEmojis += emojis;
        createTabForCategory(categoryIdx, tabIndex,
                             categoryDisplayNames[categoryIdx], emojis);
        ++tabIndex;
    }
}

QPushButton* ChooseEmojiDialog::createEmojiButton(const Emoji& emoji,
                                                  uint32_t categoryIndex,
                                                  uint32_t tabIndex)
{
    auto* pushButton = new QPushButton(this);
    pushButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    pushButton->setMinimumSize(QSize(32, 32));
    pushButton->setMaximumSize(QSize(32, 32));
    pushButton->setText(emoji.unicodeString);
    pushButton->setToolTip(emoji.name);
    pushButton->setProperty("emojiUnicodeString", emoji.unicodeString);
    pushButton->setFont(EmojiDialogSupport::emojiButtonFont(font()));
    pushButton->setFlat(true);
    pushButton->setContextMenuPolicy(Qt::CustomContextMenu);

    connect(pushButton, &QWidget::customContextMenuRequested,
            this, [this, pushButton, emoji] {
        qDebug() << "customContextMenuRequested " << pushButton->pos() << " " << emoji.name;

        QMenu menu(this);
        if (favorites.contains(EmojiInfo::findByName(emoji.name))) {
            qDebug() << emoji.name << " is in favorites map";
            menu.addAction("Remove from favorites", [this, emoji] {
                EmojiID emojiID = EmojiInfo::findByName(emoji.name);
                auto it = favorites.find(emojiID);
                if (it != favorites.end()) {
                    favorites.erase(it);
                    saveEmojiFavorites();
                    updateFavoritesTab();
                }
            });
        } else {
            menu.addAction("Add to favorites", [this, emoji] {
                EmojiID emojiID = EmojiInfo::findByName(emoji.name);
                qDebug() << "Add to favorites: " << emoji.name << " " << emojiID.seq;
                favorites.insert(emojiID, emoji);
                saveEmojiFavorites();
                updateFavoritesTab();
            });
        }

        menu.exec(pushButton->mapToGlobal(QPoint(pushButton->width(), 0)));
    });

    // Custom emojis encode their qrc image in the string. Render the image on
    // the button while retaining the original string as semantic button data.
    QString str(emoji.unicodeString);
    int found1 = str.indexOf(QLatin1Char('"'));
    if (found1 != -1) {
        ++found1;
        int found2 = str.indexOf(QLatin1Char('"'), found1);
        if (found2 != -1) {
            QString path(str.mid(found1, found2 - found1));
            path.replace(QStringLiteral("qrc://"), QStringLiteral(":/"));
            qDebug() << "Use path " << path;
            QIcon icon(QPixmap::fromImage(QImage(path)));
            pushButton->clear();
            pushButton->setIcon(icon);
            pushButton->setIconSize(QSize(24, 24));

            if (categoryIndex == EmojiCategory::custom
                && emoji.name == QStringLiteral("mattermost")
                && tabIndex < static_cast<uint32_t>(ui->tabWidget->count())) {
                ui->tabWidget->setTabIcon(static_cast<int>(tabIndex), icon);
            }
        }
    }

    connect(pushButton, &QPushButton::clicked, this, [this, pushButton] {
        selectedEmoji.name = pushButton->toolTip();
        selectedEmoji.unicodeString = pushButton->property("emojiUnicodeString").toString();
        accept();
    });

    return pushButton;
}

void ChooseEmojiDialog::createTabForCategory(uint32_t categoryIndex,
                                             uint32_t tabIndex,
                                             const QString& tabName,
                                             const QVector<Emoji>& emojis)
{
    int row = 0;
    int column = 0;

    QGridLayout* gridLayout = createTab(categoryIndex, static_cast<int>(tabIndex));

    // For the people category, add a combobox for selecting skin tone.
    if (categoryIndex == EmojiCategory::people) {
        addSkinToneComboBox(ui->tabWidget->widget(static_cast<int>(tabIndex)),
                            gridLayout, categoryIndex);
        peopleEmojiButtons.reserve(emojis.size());
        row = 1;
    }

    for (const Emoji& emoji : emojis) {
        QPushButton* pushButton = createEmojiButton(emoji, categoryIndex, tabIndex);
        gridLayout->addWidget(pushButton, row, column, 1, 1);

        if (categoryIndex == EmojiCategory::people) {
            peopleEmojiButtons.push_back(pushButton);
        }

        ++column;
        if (column == itemsPerRow) {
            column = 0;
            ++row;
        }
    }

    QString iconString;
    if (categoryIndex < EmojiCategory::COUNT
        && categoryIndex != EmojiCategory::custom
        && !emojis.isEmpty()
        && indexForCategoryTab[categoryIndex] < static_cast<uint32_t>(emojis.size())) {
        iconString = emojis[static_cast<int>(indexForCategoryTab[categoryIndex])].unicodeString;
    }
    ui->tabWidget->setTabText(static_cast<int>(tabIndex), iconString + tabName);

    if (row == 0 && column < itemsPerRow) {
        auto* horizontalSpacer = new QSpacerItem(40, 20,
                                                  QSizePolicy::Expanding,
                                                  QSizePolicy::Minimum);
        gridLayout->addItem(horizontalSpacer, 0, column, 1, itemsPerRow - column);
    }

    auto* verticalSpacer = new QSpacerItem(20, 40,
                                            QSizePolicy::Minimum,
                                            QSizePolicy::Expanding);
    gridLayout->addItem(verticalSpacer, row + 1, 0, 1, 1);
}

void ChooseEmojiDialog::updateSearchResults(const QString& text)
{
    const QString needle = EmojiDialogSupport::normalizeSearchTerm(text);
    if (needle.isEmpty()) {
        removeSearchTab();
        return;
    }

    if (searchReturnTabIndex < 0) {
        searchReturnTabIndex = ui->tabWidget->currentIndex();
    }
    if (searchTab) {
        const int oldIndex = ui->tabWidget->indexOf(searchTab);
        if (oldIndex >= 0) {
            ui->tabWidget->removeTab(oldIndex);
        }
        delete searchTab;
        searchTab = nullptr;
    }

    QVector<Emoji> prefixMatches;
    QVector<Emoji> otherMatches;
    prefixMatches.reserve(searchableEmojis.size());
    otherMatches.reserve(searchableEmojis.size());

    for (const Emoji& emoji : searchableEmojis) {
        if (!EmojiDialogSupport::matchesSearch(emoji.name, needle)) {
            continue;
        }
        QString normalizedName = EmojiDialogSupport::normalizeSearchTerm(emoji.name);
        if (normalizedName.startsWith(needle)) {
            prefixMatches.push_back(emoji);
        } else {
            otherMatches.push_back(emoji);
        }
    }

    searchTab = new QWidget;
    searchTab->setObjectName(QStringLiteral("emojiSearchResults"));
    auto* gridLayout = new QGridLayout(searchTab);
    gridLayout->setSpacing(0);
    gridLayout->setContentsMargins(0, 0, 0, 0);

    const int totalMatches = prefixMatches.size() + otherMatches.size();
    int shown = 0;
    int row = 0;
    int column = 0;
    auto addMatches = [&](const QVector<Emoji>& matches) {
        for (const Emoji& emoji : matches) {
            if (shown >= maxSearchResults) {
                break;
            }
            QPushButton* button = createEmojiButton(emoji, searchCategory,
                                                     static_cast<uint32_t>(ui->tabWidget->count()));
            gridLayout->addWidget(button, row, column, 1, 1);
            ++shown;
            ++column;
            if (column == itemsPerRow) {
                column = 0;
                ++row;
            }
        }
    };
    addMatches(prefixMatches);
    if (shown < maxSearchResults) {
        addMatches(otherMatches);
    }

    if (totalMatches == 0) {
        auto* emptyLabel = new QLabel(tr("No emoji found"), searchTab);
        gridLayout->addWidget(emptyLabel, 0, 0, 1, itemsPerRow, Qt::AlignCenter);
        row = 1;
    } else if (shown < totalMatches) {
        auto* limitLabel = new QLabel(
            tr("Showing first %1 of %2 matches — refine the search")
                .arg(shown).arg(totalMatches), searchTab);
        gridLayout->addWidget(limitLabel, row + 1, 0, 1, itemsPerRow, Qt::AlignLeft);
        ++row;
    }

    auto* verticalSpacer = new QSpacerItem(20, 40,
                                            QSizePolicy::Minimum,
                                            QSizePolicy::Expanding);
    gridLayout->addItem(verticalSpacer, row + 1, 0, 1, 1);

    const int searchIndex = ui->tabWidget->addTab(searchTab, tr("Search"));
    ui->tabWidget->setCurrentIndex(searchIndex);
}

void ChooseEmojiDialog::removeSearchTab()
{
    if (!searchTab) {
        searchReturnTabIndex = -1;
        return;
    }

    const int searchIndex = ui->tabWidget->indexOf(searchTab);
    if (searchIndex >= 0) {
        ui->tabWidget->removeTab(searchIndex);
    }
    delete searchTab;
    searchTab = nullptr;

    if (searchReturnTabIndex >= 0 && ui->tabWidget->count() > 0) {
        ui->tabWidget->setCurrentIndex(
            qBound(0, searchReturnTabIndex, ui->tabWidget->count() - 1));
    }
    searchReturnTabIndex = -1;
}

void ChooseEmojiDialog::addSkinToneComboBox(QWidget* tab,
                                             QGridLayout* gridLayout,
                                             uint32_t categoryIdx)
{
    QLabel* label = new QLabel(tab);
    label->setText("Skin Tone:");
    gridLayout->addWidget(label, 0, 0, 1, 2);

    skinToneComboBox = new QComboBox(tab);
    skinToneComboBox->setToolTip(
        "Emojis from this category have a 'Skin Tone' property,\n"
        "which can modify the skin color, making it different from the classic one (yellow)");

    for (int i = 0; i < EmojiSkinTone::COUNT; ++i) {
        skinToneComboBox->addItem(EmojiSkinTone::descriptionString[i], i);
    }

    connect(skinToneComboBox, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this, categoryIdx](int index) {
        qDebug() << "Set skin tone " << index;
        QVector<Emoji> emojis = EmojiInfo::getAllEmojis(categoryIdx, index);

        for (int i = 0; i < emojis.size(); ++i) {
            if (i >= peopleEmojiButtons.size()) {
                qDebug() << "Emoji index " << i
                         << " exceeds peopleEmojiButtons count" << peopleEmojiButtons.size();
                return;
            }

            peopleEmojiButtons[i]->setToolTip(emojis[i].name + EmojiSkinTone::nameString[index]);
            peopleEmojiButtons[i]->setText(emojis[i].unicodeString);
            peopleEmojiButtons[i]->setProperty("emojiUnicodeString", emojis[i].unicodeString);
        }
    });

    gridLayout->addWidget(skinToneComboBox, 0, 3, 1, 4);
}

} /* namespace Mattermost */
