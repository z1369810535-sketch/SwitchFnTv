/*
    Copyright 2023 dragonflylee
*/

#include "tab/media_collection.hpp"
#include "api/jellyfin.hpp"
#include "api/fntv.hpp"
#include "utils/image.hpp"
#include "view/video_card.hpp"
#include "view/video_source.hpp"
#include "view/media_filter.hpp"
#include "view/auto_tab_frame.hpp"
#include "view/h_recycling.hpp"
#include "tab/suggest_show.hpp"
#include "tab/suggest_movie.hpp"
#include "tab/song_list.hpp"
#include "utils/keybind.hpp"
#include "view/recycling_grid.hpp"
#include <fmt/ranges.h>

using namespace brls::literals;  // for _i18n

std::map<std::string, std::string> MediaCollection::customPrefs;

class GenresDataSource : public RecyclingGridDataSource {
public:
    using MediaList = std::vector<jellyfin::Genres>;

    explicit GenresDataSource(const MediaList& r, const std::string& itemId, const std::string& itemType)
        : list(std::move(r)), itemId(itemId), itemType(itemType) {
        brls::Logger::debug("GenresDataSource: create {}", r.size());
    }

    size_t getItemCount() override { return this->list.size(); }

    RecyclingGridItem* cellForRow(RecyclingView* recycler, size_t index) override {
        VideoCardCell* cell = dynamic_cast<VideoCardCell*>(recycler->dequeueReusableCell("Cell"));
        auto& item = this->list.at(index);
        cell->labelTitle->setText(item.Name);
        cell->labelExt->setVisibility(brls::Visibility::GONE);
        auto it = item.ImageTags.find(jellyfin::imageTypePrimary);
        if (it != item.ImageTags.end()) {
            Image::load(cell->picture, jellyfin::apiPrimaryImage, item.Id,
                HTTP::encode_form({{"tag", it->second}, {"maxWidth", "300"}}));
        }
        return cell;
    }

    void onItemSelected(brls::Box* recycler, size_t index) override {
        auto& item = this->list.at(index);
        recycler->present(new MediaCollection(this->itemId, this->itemType, item.Id));
    }

    void clearData() override { this->list.clear(); }

private:
    MediaList list;
    std::string itemId;
    std::string itemType;
};

class GenresTab : public RecyclingGrid {
public:
    GenresTab(const std::string& itemId, const std::string& itemType) {
        this->setGrow(1.f);
        this->registerCell("Cell", VideoCardCell::create);
        this->estimatedRowHeight = brls::getStyle().getMetric("app/album/height");
        this->spanCount = brls::getStyle().getMetric("app/grid/6");

        std::string query = HTTP::encode_form({
            {"userId", AppConfig::instance().getUserId()},
            {"parentId", itemId},
            {"includeItemTypes", itemType},
            {"enableImageTypes", "Primary"},
            {"recursive", "true"},
        });

        ASYNC_RETAIN
        jellyfin::getJSON<jellyfin::Result<jellyfin::Genres>>(
            [ASYNC_TOKEN, itemId, itemType](const jellyfin::Result<jellyfin::Genres>& r) {
                ASYNC_RELEASE
                this->setDataSource(new GenresDataSource(r.Items, itemId, itemType));
            },
            [ASYNC_TOKEN](const std::string& ex) {
                ASYNC_RELEASE
                this->setError(ex);
            },
            jellyfin::apiGenres, query);
    }
};

class ArtistsTab : public RecyclingGrid {
public:
    ArtistsTab(const std::string& itemId) : itemId(itemId) {
        this->setGrow(1.f);
        this->registerCell("Cell", VideoCardCell::create);
        this->estimatedRowHeight = brls::getStyle().getMetric("app/album/height");
        this->spanCount = brls::getStyle().getMetric("app/grid/6");

        this->onNextPage([this] { this->doRequest(); });
        this->doRequest();
    }

    void doRequest() {
        std::string query = HTTP::encode_form({
            {"userId", AppConfig::instance().getUserId()},
            {"parentId", this->itemId},
            {"limit", std::to_string(this->pageSize)},
            {"startIndex", std::to_string(this->start)},
            {"enableImageTypes", "Primary"},
            {"recursive", "true"},
        });

        ASYNC_RETAIN
        jellyfin::getJSON<jellyfin::Result<jellyfin::Episode>>(
            [ASYNC_TOKEN](const jellyfin::Result<jellyfin::Episode>& r) {
                ASYNC_RELEASE
                this->start = r.StartIndex + this->pageSize;
                if (r.TotalRecordCount == 0) {
                    this->clearData();
                } else if (r.StartIndex == 0) {
                    this->setDataSource(new VideoDataSource(r.Items, this->itemId));
                } else if (r.Items.size() > 0) {
                    auto dataSrc = dynamic_cast<VideoDataSource*>(this->getDataSource());
                    dataSrc->appendData(r.Items);
                    this->notifyDataChanged();
                }
            },
            [ASYNC_TOKEN](const std::string& ex) {
                ASYNC_RELEASE
                this->setError(ex);
            },
            jellyfin::apiArtists, query);
    }

private:
    std::string itemId;
    size_t start = 0;
    size_t pageSize = 60;
};

namespace {
int sortIndexOf(const std::string& column) {
    const auto& options = fntv::sortOptions();
    for (size_t i = 0; i < options.size(); ++i)
        if (options[i].column == column) return static_cast<int>(i);
    return 0;
}
}  // namespace

// Libraries open newest first like the Feiniu web client; folders keep file-name order
// (episodes stored as 01.mkv, 02.mkv... stay in sequence). Both are remembered per session.
MediaCollection::SortPref MediaCollection::libraryPref = {sortIndexOf(fntv::SORT_ADDED), 1};
MediaCollection::SortPref MediaCollection::folderPref = {sortIndexOf(fntv::SORT_NAME), 0};

void MediaCollection::clearPref() {
    customPrefs.clear();
    libraryPref = {sortIndexOf(fntv::SORT_ADDED), 1};
    folderPref = {sortIndexOf(fntv::SORT_NAME), 0};
}

MediaCollection::MediaCollection(
    const std::string& itemId, const std::string& itemType, const std::string& genresId, const std::string& title)
    : itemId(itemId), genresId(genresId), itemType(itemType), title(title), startIndex(0) {
    brls::Logger::debug("MediaCollection: create {} type {}", itemId, itemType);
    this->inflateFromXMLRes("xml/tabs/media.xml");

    this->pageSize = this->recycler->spanCount * 3;
    if (itemType == jellyfin::mediaTypeMusicAlbum) {
        this->recycler->estimatedRowHeight = brls::getStyle().getMetric("app/album/height");
    } else if (itemType == jellyfin::mediaTypeBook) {
        this->recycler->estimatedRowHeight = brls::getStyle().getMetric("app/books/height");
    }

    const SortPref& pref = this->isFolder() ? folderPref : libraryPref;
    this->sortIndex = pref.sort;
    this->orderIndex = pref.order;

    this->recycler->registerAction("hints/refresh"_i18n, brls::BUTTON_BACK, [this](...) {
        this->doRequest(true);
        return true;
    });

    this->registerAction(KeyBind::getRefresh(), [this](...) {
        this->doRequest(true);
        return true;
    });

    this->recycler->registerCell("Cell", VideoCardCell::create);
    this->recycler->onNextPage([this]() { this->doRequest(); });

    // Y sorts every library and folder (it used to open a Jellyfin filter whose choice was
    // ignored, and was not registered at all while "sync settings" was on).
    this->registerAction("main/media/sort"_i18n, brls::BUTTON_Y, [this](...) {
        this->openSort();
        return true;
    });

    this->updateHeader();
    this->doRequest();
}

brls::View* MediaCollection::getDefaultFocus() { return this->recycler; }

bool MediaCollection::isFolder() const { return this->itemType == jellyfin::mediaTypeFolder; }

void MediaCollection::openSort() {
    MediaFilter* filter = new MediaFilter(this->sortIndex, this->orderIndex, [this](int sort, int order) {
        this->sortIndex = sort;
        this->orderIndex = order;
        (this->isFolder() ? folderPref : libraryPref) = {sort, order};
        this->doRequest(true);
    });
    brls::Application::pushActivity(new brls::Activity(filter));
}

void MediaCollection::updateHeader() {
    this->header->setTitle(this->title.empty() ? std::string("媒体库") : this->title);
    const auto& options = fntv::sortOptions();
    std::string subtitle;
    if (this->totalCount >= 0) subtitle = "共 " + std::to_string(this->totalCount) + " 项 · ";
    if (this->sortIndex >= 0 && this->sortIndex < static_cast<int>(options.size()))
        subtitle += brls::getStr(options[this->sortIndex].label) + " " +
                    (this->orderIndex ? "main/media/descending"_i18n : "main/media/ascending"_i18n);
    this->header->setSubtitle(subtitle);
}

void MediaCollection::doRequest(bool refresh) {
    if (refresh) {
        ++this->requestGeneration;
        this->startIndex = 0;
        this->loading = false;
        this->hasMore = true;
        this->totalCount = -1;
        this->updateHeader();
        this->recycler->showSkeleton();
    }
    if (this->loading || !this->hasMore) return;
    this->loading = true;
    const auto generation = this->requestGeneration;
    const std::string parent = this->itemId;
    const std::string type = this->itemType;
    const size_t start = this->startIndex;
    const size_t page = this->pageSize ? this->pageSize : 30;
    const auto& options = fntv::sortOptions();
    const std::string column =
        this->sortIndex >= 0 && this->sortIndex < static_cast<int>(options.size()) ? options[this->sortIndex].column : "";
    const std::string direction = this->orderIndex ? "DESC" : "ASC";

    ASYNC_RETAIN
    fntv::async<jellyfin::Result<jellyfin::Episode>>(
        [parent, type, start, page, column, direction] {
            return fntv::listItems(parent, start, page, type, column, direction);
        },
        [ASYNC_TOKEN, generation, page](const jellyfin::Result<jellyfin::Episode>& r) {
            ASYNC_RELEASE
            if (generation != this->requestGeneration) return;
            this->loading = false;
            this->startIndex = r.StartIndex + page;
            this->hasMore = !r.Items.empty();
            if (r.StartIndex == 0) {
                this->totalCount = r.TotalRecordCount;
                this->updateHeader();
                if (r.Items.empty()) {
                    this->recycler->setEmpty("这里还没有可显示的影片");
                } else {
                    this->recycler->setDataSource(new VideoDataSource(r.Items));
                    brls::Application::giveFocus(this->recycler);
                }
            } else if (r.Items.size() > 0) {
                auto dataSrc = dynamic_cast<VideoDataSource*>(this->recycler->getDataSource());
                if (dataSrc) {
                    this->hasMore = dataSrc->appendData(r.Items) > 0;
                    if (this->hasMore) this->recycler->notifyDataChanged();
                }
            }
        },
        [ASYNC_TOKEN, generation](const std::string& ex) {
            ASYNC_RELEASE
            if (generation != this->requestGeneration) return;
            this->loading = false;
            if (this->startIndex > 0) {
                brls::Application::notify(ex);
            } else {
                // Keep the way out visible: "-" reloads, B goes back.
                this->recycler->setError(ex + "\n按 - 键重试");
            }
        });
}
