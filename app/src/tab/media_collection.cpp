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

MediaCollection::MediaCollection(const std::string& itemId, const std::string& itemType, const std::string& genresId)
    : itemId(itemId), genresId(genresId), itemType(itemType), startIndex(0) {
    brls::Logger::debug("MediaCollection: create {} type {}", itemId, itemType);
    this->inflateFromXMLRes("xml/tabs/media.xml");

    this->pageSize = this->recycler->spanCount * 3;
    if (itemType == jellyfin::mediaTypeMusicAlbum) {
        this->recycler->estimatedRowHeight = brls::getStyle().getMetric("app/album/height");
    } else if (itemType == jellyfin::mediaTypeBook) {
        this->recycler->estimatedRowHeight = brls::getStyle().getMetric("app/books/height");
    }

    std::string serverUrl = AppConfig::instance().getUrl();
    this->prefKey = fmt::format("{}/web/index.html{}", serverUrl, itemType);
    std::transform(this->prefKey.begin(), this->prefKey.end(), this->prefKey.begin(), ::tolower);

    this->recycler->registerAction("hints/refresh"_i18n, brls::BUTTON_BACK, [this](...) {
        this->startIndex = 0;
        this->recycler->showSkeleton();
        this->doRequest();
        return true;
    });

    this->registerAction(KeyBind::getRefresh(), [this](...) {
        this->startIndex = 0;
        this->recycler->showSkeleton();
        this->doRequest();
        return true;
    });

    this->recycler->registerCell("Cell", VideoCardCell::create);
    this->recycler->onNextPage([this]() { this->doRequest(); });

    if (this->itemType == jellyfin::mediaTypePlaylist) {
        this->doRequest();
    } else if (AppConfig::SYNC) {
        if (MediaCollection::customPrefs.empty()) {
            this->doPreferences();
        } else {
            this->loadFilter();
            this->doRequest();
        }
    } else {
        this->registerAction("main/media/sort"_i18n, brls::BUTTON_Y, [this](...) {
            MediaFilter* filter = new MediaFilter();
            filter->getEvent()->subscribe([this]() {
                this->startIndex = 0;
                this->recycler->showSkeleton();
                this->doRequest();
            });
            brls::Application::pushActivity(new brls::Activity(filter));
            return true;
        });

        this->doRequest();
    }
}

brls::View* MediaCollection::getDefaultFocus() { return this->recycler; }

void MediaCollection::doPreferences() {
    this->doRequest();
}

void MediaCollection::loadFilter() {}

void MediaCollection::saveFilter() {}

void MediaCollection::doRequest() {
    const std::string parent = this->itemId;
    const std::string type = this->itemType;
    const size_t start = this->startIndex;
    const size_t page = this->pageSize ? this->pageSize : 30;

    ASYNC_RETAIN
    fntv::async<jellyfin::Result<jellyfin::Episode>>(
        [parent, type, start, page] { return fntv::listItems(parent, start, page, type); },
        [ASYNC_TOKEN](const jellyfin::Result<jellyfin::Episode>& r) {
            ASYNC_RELEASE
            this->startIndex = r.StartIndex + this->pageSize;
            if (r.TotalRecordCount == 0 && r.Items.empty()) {
                this->recycler->setEmpty();
            } else if (r.StartIndex == 0) {
                this->recycler->setDataSource(new VideoDataSource(r.Items));
                brls::Application::giveFocus(this->recycler);
            } else if (r.Items.size() > 0) {
                auto dataSrc = dynamic_cast<VideoDataSource*>(this->recycler->getDataSource());
                if (dataSrc) {
                    dataSrc->appendData(r.Items);
                    this->recycler->notifyDataChanged();
                }
            }
        },
        [ASYNC_TOKEN](const std::string& ex) {
            ASYNC_RELEASE
            if (this->startIndex > 0) {
                brls::Application::notify(ex);
            } else {
                this->recycler->setError(ex);
            }
        });
}
