/**
 * @file TeamChannelsListDialog.cpp
 * @brief Lazily loaded public-channel directory for one Mattermost team.
 */

#include "TeamChannelsListDialog.h"

#include <algorithm>
#include <utility>

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPointer>
#include <QSizePolicy>
#include <QTimer>

#include "backend/Backend.h"
#include "backend/PublicChannelPaging.h"
#include "backend/Storage.h"
#include "backend/types/BackendTeam.h"
#include "info-dialogs/ChannelInfoDialog.h"
#include "ui_FilterListDialog.h"
#include "widgets/LongListWidget.h"

namespace Mattermost {
namespace {

constexpr int ChannelRowHeight = 44;
constexpr int SearchDelayMs = 200;

class PublicChannelRow final : public QWidget
{
public:
    PublicChannelRow(BackendChannel& channel,
                     std::function<void(const QPoint&)> contextMenu,
                     QWidget* parent = nullptr)
        : QWidget(parent)
        , contextMenu_(std::move(contextMenu))
    {
        setMinimumHeight(ChannelRowHeight);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setContextMenuPolicy(Qt::CustomContextMenu);

        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(8, 3, 8, 3);
        layout->setSpacing(12);

        auto* name = new QLabel(channel.display_name, this);
        name->setTextInteractionFlags(Qt::NoTextInteraction);
        name->setToolTip(channel.display_name);
        layout->addWidget(name, 2);

        auto* header = new QLabel(channel.header, this);
        header->setTextInteractionFlags(Qt::NoTextInteraction);
        header->setWordWrap(false);
        header->setToolTip(channel.header);
        layout->addWidget(header, 5);

        connect(this, &QWidget::customContextMenuRequested, this,
                [this](const QPoint& pos) {
            if (contextMenu_) {
                contextMenu_(mapToGlobal(pos));
            }
        });
    }

private:
    std::function<void(const QPoint&)> contextMenu_;
};

class PublicChannelLongList final : public LongListWidget
{
public:
    using Factory = std::function<QWidget*(int)>;

    PublicChannelLongList(Factory factory, QWidget* parent)
        : LongListWidget(parent)
        , factory_(std::move(factory))
    {
    }

protected:
    QWidget* createItemWidget(int index) override
    {
        return factory_ ? factory_(index) : nullptr;
    }

private:
    Factory factory_;
};

} // namespace

TeamChannelsListDialog::TeamChannelsListDialog(
    Backend& backend,
    const FilterListDialogConfig& cfg,
    BackendTeam& team,
    QWidget* parent)
    : FilterListDialog(parent)
    , backend(backend)
    , team(team)
{
    setupVirtualList(cfg);
    pagedItemCount = PublicChannelsPerPage;
    pageChannels.resize(pagedItemCount);
    channelList->setItemCount(pagedItemCount);
    ui->usersCountLabel->setText(tr("Loading channels…"));
}

TeamChannelsListDialog::~TeamChannelsListDialog()
{
    finishAllRequests();
}

void TeamChannelsListDialog::setupVirtualList(const FilterListDialogConfig& cfg)
{
    FilterListDialog::create(cfg);

    ui->tableWidget->hide();
    const int tableIndex = ui->verticalLayout->indexOf(ui->tableWidget);
    ui->verticalLayout->insertWidget(std::max(0, tableIndex), createHeaderRow());

    channelList = new PublicChannelLongList(
        [this](int index) { return createChannelRow(index); }, this);
    channelList->setDefaultItemHeight(ChannelRowHeight);
    channelList->setMaterializationLimit(120);
    channelList->setRequestBlockSize(32);
    channelList->setPrefetchScreens(1);
    ui->verticalLayout->insertWidget(std::max(0, tableIndex) + 1, channelList, 1);

    connect(channelList, &LongListWidget::rangeRequested, this,
            [this](int first, int last, LongListWidget::RequestReason, quint64) {
        requestChannelRange(first, last);
    });

    // FilterListDialog's legacy handler only knows about its hidden QTableWidget.
    // Replace it with server-backed search so unloaded channels remain searchable.
    QObject::disconnect(ui->filterLineEdit, nullptr, this, nullptr);
    searchTimer = new QTimer(this);
    searchTimer->setSingleShot(true);
    searchTimer->setInterval(SearchDelayMs);
    connect(searchTimer, &QTimer::timeout, this, &TeamChannelsListDialog::performSearch);
    connect(ui->filterLineEdit, &QLineEdit::textEdited, this,
            [this](const QString& text) { filterEdited(text); });
}

QWidget* TeamChannelsListDialog::createHeaderRow()
{
    auto* header = new QWidget(this);
    auto* layout = new QHBoxLayout(header);
    layout->setContentsMargins(8, 0, 8, 0);
    layout->setSpacing(12);

    auto addHeader = [layout, header](const QString& text, int stretch) {
        auto* label = new QLabel(text, header);
        QFont font = label->font();
        font.setBold(true);
        label->setFont(font);
        layout->addWidget(label, stretch);
    };
    addHeader(tr("Channel Name"), 2);
    addHeader(tr("Channel Header"), 5);
    return header;
}

QWidget* TeamChannelsListDialog::createChannelRow(int index)
{
    BackendChannel* channel = channelAt(index);
    if (!channel) {
        return nullptr;
    }
    return new PublicChannelRow(
        *channel,
        [this, channel](const QPoint& globalPos) {
            showChannelContextMenu(channel, globalPos);
        }, channelList);
}

BackendChannel* TeamChannelsListDialog::channelAt(int index) const
{
    const QVector<BackendChannel*>& channels = searchMode ? searchChannels : pageChannels;
    return index >= 0 && index < channels.size() ? channels.at(index) : nullptr;
}

void TeamChannelsListDialog::showChannelContextMenu(BackendChannel* channel,
                                                     const QPoint& globalPos)
{
    if (!channel) {
        return;
    }
    QMenu menu(this);
    menu.addAction(tr("Join this channel"), this, [this, channel] {
        backend.joinChannel(*channel);
    });
    menu.addAction(tr("View channel details"), this, [this, channel] {
        auto* dialog = new ChannelInfoDialog(*channel, this);
        dialog->show();
    });
    menu.exec(globalPos);
}

void TeamChannelsListDialog::addContextMenuActions(QMenu& menu,
                                                    const QVariant& selectedItemData)
{
    BackendChannel* channel = selectedItemData.value<BackendChannel*>();
    if (!channel) {
        return;
    }
    menu.addAction(tr("Join this channel"), this, [this, channel] {
        backend.joinChannel(*channel);
    });
    menu.addAction(tr("View channel details"), this, [this, channel] {
        auto* dialog = new ChannelInfoDialog(*channel, this);
        dialog->show();
    });
}

void TeamChannelsListDialog::setItemCountLabel(uint32_t count)
{
    ui->usersCountLabel->setText(QString::number(count)
        + (count == 1 ? tr(" channel") : tr(" channels")));
}

void TeamChannelsListDialog::updatePagedCountLabel(bool hasMore)
{
    ui->usersCountLabel->setText(QString::number(concretePagedCount)
        + (hasMore ? QStringLiteral("+ channels")
                   : (concretePagedCount == 1 ? tr(" channel") : tr(" channels"))));
}

void TeamChannelsListDialog::requestChannelRange(int first, int last)
{
    if (!channelList || first < 0 || last < first) {
        return;
    }
    if (searchMode) {
        channelList->finishRangeRequest(first, last);
        return;
    }

    PendingRange pending;
    pending.first = first;
    pending.last = last;
    const int firstPage = first / PublicChannelsPerPage;
    const int lastPage = last / PublicChannelsPerPage;
    for (int page = firstPage; page <= lastPage; ++page) {
        if (!loadedPages.contains(page)) {
            pending.pages.insert(page);
            loadPage(page);
        }
    }

    if (pending.pages.isEmpty()) {
        channelList->finishRangeRequest(first, last);
    } else {
        pendingRanges.push_back(std::move(pending));
    }
}

void TeamChannelsListDialog::loadPage(int page)
{
    if (page < 0 || loadedPages.contains(page) || inFlightPages.contains(page)) {
        return;
    }
    inFlightPages.insert(page);
    QPointer<TeamChannelsListDialog> guard(this);
    backend.retrieveTeamPublicChannelsPage(
        team.id, page, PublicChannelsPerPage,
        [guard, page](QJsonArray values) {
            if (!guard) {
                return;
            }

            guard->inFlightPages.remove(page);
            const int pageStart = page * PublicChannelsPerPage;
            if (guard->pageChannels.size() < pageStart + values.size()) {
                guard->pageChannels.resize(pageStart + values.size());
            }

            int offset = 0;
            for (const auto& value : values) {
                const QJsonObject object = value.toObject();
                if (BackendChannel::getChannelType(object) != BackendChannel::publicChannel) {
                    continue;
                }
                guard->pageStorage.emplace_back(guard->backend.getStorage(), object);
                guard->pageChannels[pageStart + offset] = &guard->pageStorage.back();
                ++offset;
            }

            // The public-channel endpoint itself only returns open channels, so
            // filtering here is defensive. Treat any omitted non-open row as a
            // short page rather than inventing unavailable logical slots.
            const int returnedCount = offset;
            guard->loadedPages.insert(page);
            guard->concretePagedCount = std::max(
                guard->concretePagedCount, pageStart + returnedCount);
            guard->pagedEndKnown = publicChannelPageProvesEnd(returnedCount);
            guard->pagedItemCount = publicChannelLogicalCountAfterPage(page, returnedCount);

            if (!guard->searchMode) {
                guard->channelList->setItemCount(guard->pagedItemCount);
                if (returnedCount > 0) {
                    guard->channelList->setRangeAvailable(
                        pageStart, pageStart + returnedCount - 1);
                }
                guard->updatePagedCountLabel(!guard->pagedEndKnown);
            }
            guard->completePage(page);
        });
}

void TeamChannelsListDialog::completePage(int page)
{
    int index = 0;
    while (index < pendingRanges.size()) {
        PendingRange& pending = pendingRanges[index];
        pending.pages.remove(page);
        if (!pending.pages.isEmpty()) {
            ++index;
            continue;
        }
        if (channelList) {
            channelList->finishRangeRequest(pending.first, pending.last);
        }
        pendingRanges.removeAt(index);
    }
}

void TeamChannelsListDialog::finishAllRequests()
{
    if (channelList) {
        for (const PendingRange& pending : std::as_const(pendingRanges)) {
            channelList->finishRangeRequest(pending.first, pending.last);
        }
    }
    pendingRanges.clear();
}

void TeamChannelsListDialog::reapplyLoadedPages()
{
    if (!channelList || searchMode) {
        return;
    }
    for (int page : std::as_const(loadedPages)) {
        const int first = page * PublicChannelsPerPage;
        const int last = std::min(first + PublicChannelsPerPage,
                                  static_cast<int>(pageChannels.size())) - 1;
        int concreteLast = last;
        while (concreteLast >= first && !pageChannels.value(concreteLast)) {
            --concreteLast;
        }
        if (concreteLast >= first) {
            channelList->setRangeAvailable(first, concreteLast);
        }
    }
}

void TeamChannelsListDialog::filterEdited(const QString& text)
{
    searchTerm = text.trimmed();
    ++searchGeneration;
    searchTimer->stop();

    if (searchTerm.isEmpty()) {
        restorePagedMode();
        return;
    }

    finishAllRequests();
    searchMode = true;
    channelList->setItemCount(0);
    searchStorage.clear();
    searchChannels.clear();
    ui->usersCountLabel->setText(tr("Searching…"));
    searchTimer->start();
}

void TeamChannelsListDialog::performSearch()
{
    if (searchTerm.isEmpty()) {
        return;
    }
    const int generation = searchGeneration;
    const QString term = searchTerm;
    QPointer<TeamChannelsListDialog> guard(this);
    backend.searchTeamPublicChannels(team.id, term,
        [guard, generation](QJsonArray results) {
            if (guard) {
                guard->enterSearchResults(std::move(results), generation);
            }
        });
}

void TeamChannelsListDialog::enterSearchResults(QJsonArray results, int generation)
{
    if (generation != searchGeneration || searchTerm.isEmpty()) {
        return;
    }

    channelList->setItemCount(0);
    searchStorage.clear();
    searchChannels.clear();
    for (const auto& value : results) {
        const QJsonObject object = value.toObject();
        if (BackendChannel::getChannelType(object) != BackendChannel::publicChannel) {
            continue;
        }
        searchStorage.emplace_back(backend.getStorage(), object);
        searchChannels.push_back(&searchStorage.back());
    }

    channelList->setItemCount(searchChannels.size());
    if (!searchChannels.isEmpty()) {
        channelList->setRangeAvailable(0, searchChannels.size() - 1);
    }
    setItemCountLabel(static_cast<uint32_t>(searchChannels.size()));
}

void TeamChannelsListDialog::restorePagedMode()
{
    finishAllRequests();
    searchMode = false;
    channelList->setItemCount(0);
    searchStorage.clear();
    searchChannels.clear();
    channelList->setItemCount(pagedItemCount);
    reapplyLoadedPages();
    updatePagedCountLabel(!pagedEndKnown);
}

} // namespace Mattermost
