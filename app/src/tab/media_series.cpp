/*
    Copyright 2023 dragonflylee
*/

#include "activity/player_view.hpp"
#include "api/jellyfin.hpp"
#include "api/fntv.hpp"
#include "tab/media_series.hpp"
#include "view/h_recycling.hpp"
#include "view/auto_tab_frame.hpp"
#include "view/icon_button.hpp"
#include "view/svg_image.hpp"
#include "view/text_box.hpp"
#include "view/video_card.hpp"
#include "view/people_source.hpp"
#include "view/video_source.hpp"
#include "view/presenter.hpp"
#include "view/context_menu.hpp"
#include "utils/keybind.hpp"
#include "utils/dialog.hpp"
#include "utils/misc.hpp"
#include <fmt/ranges.h>

using namespace brls::literals;  // for _i18n

MediaSeries::MediaSeries(const jellyfin::Episode& item) {
    brls::Logger::debug("Tab MediaSeries: create");
    // Inflate the tab from the XML file
    this->inflateFromXMLRes("xml/tabs/series.xml");

    if (item.Type == jellyfin::mediaTypeSeries) {
        this->seriesId = item.Id;
    } else if (item.SeriesId.is_string()) {
        this->seriesId = item.SeriesId.get<std::string>();
    }

    this->labelTitle->setText(item.Name);
    this->labelStatus->setText("正在读取详情…");
    this->labelOverview->setText("正在加载简介…");
    this->labelOverview->setFocusable(true);
    this->labelOverview->registerClickAction([this](...) { Dialog::show(this->labelOverview->getFullText()); return true; });
    this->bannerBox->setVisibility(brls::Visibility::GONE);
    this->contentRow->setMarginTop(24);
    this->contentInfo->setMarginTop(0);
    this->imagePoster->getParent()->setMarginTop(0);
    fntv::loadPoster(this->imagePoster, item);
    this->registerAction("刷新详情", brls::BUTTON_Y, [this](...) { this->doRequest(); return true; });
    this->seasons->registerCell("Cell", VideoCardCell::create);
    this->people->registerCell("Cell", MediaCardCell::create);
    this->similar->registerCell("Cell", VideoCardCell::create);
    this->special->registerCell("Cell", VideoCardCell::create);

    if (brls::Application::getThemeVariant() == brls::ThemeVariant::LIGHT) {
        this->imageFade->setImageFromRes("img/fade-bottom-light.png");
    }
    // the buttons and the seasons row have no geometric overlap:
    // explicit route (cf. media_movie.cpp)
    this->btnPlay->setCustomNavigationRoute(brls::FocusDirection::DOWN, "series/seasons");
    this->btnDownload->setCustomNavigationRoute(brls::FocusDirection::DOWN, "series/seasons");

    this->btnPlay->registerClickAction([this](...) {
        this->doPlay();
        return true;
    });
    this->btnDownload->registerClickAction([this](...) {
        this->doDownloadSeries();
        return true;
    });

    auto& dm = DownloadManager::instance();
    this->updateDownloadButton();
    this->statusSub = dm.getStatusEvent()->subscribe(
        [this](const std::string& id, DownloadStatus status) { this->updateDownloadButton(); });

    this->updateFavoriteButton(item.UserData.IsFavorite);
    this->btnFavorite->registerClickAction([this](...) {
        if (this->isFavorite)
            this->unFavorite();
        else
            this->doFavorite();
        return true;
    });

    this->doSeason();
    this->updateResume();
    this->doSeries();
    this->doSimilar();
    this->doSpecial();
}

MediaSeries::~MediaSeries() {
    brls::Logger::debug("Tab MediaSeries: delete");
    auto& dm = DownloadManager::instance();
    dm.getStatusEvent()->unsubscribe(this->statusSub);
    Image::cancel(this->imageLogo);
    Image::cancel(this->imagePoster);
    Image::cancel(this->imageBackdrop);
}

void MediaSeries::doRequest() {
    // after playback: next episode (Play button) + watched states of the
    // season cards; the episodes are refreshed by MediaSeason itself
    this->doSeason();
    this->doSeries();
    this->updateResume();
}

void MediaSeries::doSeries() {
    const auto id = this->seriesId;
    ASYNC_RETAIN
    fntv::async<jellyfin::Detail>(
        [id] { return fntv::getDetail(id); },
        [ASYNC_TOKEN](const jellyfin::Detail& r) {
            ASYNC_RELEASE
            if (!r.Name.empty()) this->labelTitle->setText(r.Name);
            this->labelYear->setText(r.ProductionYear ? std::to_string(r.ProductionYear) : "");
            this->labelYear->getParent()->setVisibility(r.ProductionYear ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
            this->parentalRating->getParent()->setVisibility(brls::Visibility::GONE);
            if (r.CommunityRating == 0.f) {
                this->labelRating->getParent()->setVisibility(brls::Visibility::GONE);
            } else {
                this->labelRating->setText(fmt::format("{:.1f}", r.CommunityRating));
                this->labelRating->getParent()->setVisibility(brls::Visibility::VISIBLE);
            }
            this->labelOverview->setText(r.Overview.empty() ? "暂无简介" : r.Overview);
            if (r.Genres.empty()) this->labelGenres->setVisibility(brls::Visibility::GONE);
            else {
                this->labelGenres->setText(fmt::format("{}", fmt::join(r.Genres, ", ")));
                this->labelGenres->setVisibility(brls::Visibility::VISIBLE);
            }
            if (r.People.empty()) {
                this->labelPeople->setVisibility(brls::Visibility::GONE);
                this->people->setVisibility(brls::Visibility::GONE);
            } else {
                this->labelPeople->setVisibility(brls::Visibility::VISIBLE);
                this->people->setVisibility(brls::Visibility::VISIBLE);
                this->people->setDataSource(new PeopleDataSource(r.People));
            }
            this->updateFavoriteButton(r.UserData.IsFavorite);
            fntv::loadPoster(this->imagePoster, r);
            const bool backdrop = !r.BackdropImageTags.empty();
            this->bannerBox->setVisibility(backdrop ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
            if (backdrop) fntv::loadImagePath(this->imageBackdrop, r.BackdropImageTags.front());
            this->contentRow->setMarginTop(backdrop ? -100 : 24);
            this->contentInfo->setMarginTop(backdrop ? 110 : 0);
            this->imagePoster->getParent()->setMarginTop(0);
            this->btnDownload->setVisibility(brls::Visibility::GONE);
        },
        [ASYNC_TOKEN](const std::string& ex) {
            ASYNC_RELEASE
            this->labelPeople->setVisibility(brls::Visibility::GONE);
            this->people->setVisibility(brls::Visibility::GONE);
            this->labelStatus->setText("详情加载失败，按 Y 重试");
            this->labelOverview->setText(ex);
            brls::Logger::warning("doSeries {}", ex);
        });
}

void MediaSeries::doSeason() {
    const auto id = this->seriesId;
    ASYNC_RETAIN
    fntv::async<jellyfin::Result<jellyfin::Episode>>(
        [id] {
            auto r = fntv::listSeasons(id);
            if (r.Items.empty()) r = fntv::listEpisodes(id);
            return r;
        },
        [ASYNC_TOKEN](const jellyfin::Result<jellyfin::Episode>& r) {
            ASYNC_RELEASE
            if (r.Items.empty()) {
                this->labelSeasons->setSubtitle("暂无可用的季或剧集");
                this->seasons->setDataSource(new VideoDataSource(r.Items));
                return;
            }
            this->labelSeasons->setVisibility(brls::Visibility::VISIBLE);
            this->seasons->setVisibility(brls::Visibility::VISIBLE);
            this->labelSeasons->setSubtitle(std::to_string(r.Items.size()));
            this->seasons->setDataSource(new VideoDataSource(r.Items, this->seriesId));
        },
        [ASYNC_TOKEN](const std::string& ex) {
            ASYNC_RELEASE
            this->labelSeasons->setSubtitle(ex);
            brls::Application::notify(ex);
            brls::Logger::warning("doSeason {}", ex);
        });
}

void MediaSeries::doSimilar() {
    this->similar->setVisibility(brls::Visibility::GONE);
    this->labelSimilar->setVisibility(brls::Visibility::GONE);
}


void MediaSeries::doSpecial() {
    this->special->setVisibility(brls::Visibility::GONE);
    this->labelSpecial->setVisibility(brls::Visibility::GONE);
}

void MediaSeries::doPlay() {
    // The episode list is already loaded by updateResume(): start at once instead of fetching
    // every season again (and a second press can no longer open two players).
    if (this->episodesReady) {
        this->startPlay();
        return;
    }
    this->playPending = true;
    this->labelStatus->setText("正在读取剧集…");
    if (!this->episodesLoading) this->updateResume();
}

void MediaSeries::startPlay() {
    this->playPending = false;
    if (this->episodes.empty()) {
        brls::Application::notify("暂无可播放的剧集");
        return;
    }
    const auto& item = this->episodes[fntv::selectResumeEpisode(this->episodes)];
    PlayerView* view = new PlayerView(item);
    view->setTitie(fntv::episodeTitle(item));
    view->setSeries(this->seriesId, this->episodes);
    brls::sync([view]() { brls::Application::giveFocus(view); });
}

void MediaSeries::updateResume() {
    const auto id = this->seriesId;
    const auto generation = ++this->episodesGeneration;
    this->episodesLoading = true;
    ASYNC_RETAIN
    fntv::async<jellyfin::Result<jellyfin::Episode>>(
        [id] { return fntv::listSeriesEpisodes(id); },
        [ASYNC_TOKEN, generation](const jellyfin::Result<jellyfin::Episode>& r) {
            ASYNC_RELEASE
            if (generation != this->episodesGeneration) return;
            this->episodesLoading = false;
            this->episodes = r.Items;
            this->episodesReady = true;
            if (r.Items.empty()) {
                this->labelStatus->setText("暂无可播放剧集，按 Y 刷新");
                this->btnPlay->setText("main/media/play"_i18n);
                if (this->playPending) this->startPlay();
                return;
            }
            auto item = r.Items[fntv::selectResumeEpisode(r.Items)];
            fntv::applyLocalProgress(item);
            const auto seconds = item.UserData.PlaybackPositionTicks / jellyfin::PLAYTICKS;
            const std::string episode = fntv::episodeTitle(item, false);
            this->btnPlay->setText(fmt::format("{} {}", seconds > 0 ? "继续播放" : "播放", episode));
            this->labelStatus->setText(fmt::format("共 {} 集 · {}{}", r.Items.size(), episode,
                seconds > 0 ? " · 上次看到 " + misc::sec2Time(seconds) : ""));
            if (this->playPending) this->startPlay();
        },
        [ASYNC_TOKEN, generation](const std::string& ex) {
            ASYNC_RELEASE
            if (generation != this->episodesGeneration) return;
            this->episodesLoading = false;
            this->labelStatus->setText("读取剧集失败，按 Y 重试");
            if (this->playPending) {
                this->playPending = false;
                brls::Application::notify(ex);
            }
            brls::Logger::warning("series resume: {}", ex);
        });
}

void MediaSeries::doDownloadSeries() {
    ASYNC_RETAIN
    jellyfin::getJSON<jellyfin::Result<jellyfin::Episode>>(
        [ASYNC_TOKEN](const jellyfin::Result<jellyfin::Episode>& r) {
            ASYNC_RELEASE
            auto& dm = DownloadManager::instance();
            std::vector<std::string> wanted;
            for (auto& item : r.Items) {
                if (dm.findItem(item.Id) > DownloadStatus::Completed) wanted.push_back(item.Id);
            }
            if (wanted.empty()) {
                brls::Application::notify("main/download/completed"_i18n);
                return;
            }
            Dialog::cancelable(
                fmt::format(fmt::runtime("main/download/confirm_season"_i18n), wanted.size()), [wanted]() {
                    auto& dm = DownloadManager::instance();
                    int qi = AppConfig::instance().getValueIndex(AppConfig::DOWNLOAD_QUALITY);
                    for (auto& key : wanted) dm.addDownload(key, static_cast<DownloadQuality>(qi));
                    brls::Application::notify("main/download/queued"_i18n);
                });
        },
        [ASYNC_TOKEN](const std::string& ex) {
            ASYNC_RELEASE
            brls::Application::notify(ex);
        },
        jellyfin::apiShowEpisodes, this->seriesId, "");
}

void MediaSeries::updateDownloadButton() {
    auto& dm = DownloadManager::instance();
    auto it = dm.findSeries(this->seriesId);
    if (it.first == 0) {
        this->btnDownload->setText("main/download/start"_i18n);
    } else if (it.first == it.second) {
        this->btnDownload->setText("main/download/completed"_i18n);
    } else {
        this->btnDownload->setText(fmt::format("{} {}/{}", "main/download/downloading"_i18n, it.second, it.first));
    }
}

bool MediaSeries::doFavorite() {
    const auto id = this->seriesId;
    const bool favorite = !this->isFavorite;
    ASYNC_RETAIN
    fntv::async<bool>(
        [id, favorite] { return fntv::setFavorite(id, favorite); },
        [ASYNC_TOKEN](bool favorite) {
            ASYNC_RELEASE
            this->updateFavoriteButton(favorite);
        },
        [ASYNC_TOKEN](const std::string& ex) {
            ASYNC_RELEASE
            brls::Application::notify(ex);
        });
    return true;
}

bool MediaSeries::unFavorite() { return this->doFavorite(); }

void MediaSeries::updateFavoriteButton(bool favorite) {
    this->isFavorite = favorite;
    if (favorite) {
        this->btnFavorite->setIcon("icon/ico-heart.svg");
        this->btnFavorite->setText("main/media/del_favorite"_i18n);
    } else {
        this->btnFavorite->setIcon("icon/ico-heart-gray.svg");
        this->btnFavorite->setText("main/media/add_favorite"_i18n);
    }
}
