#include "activity/player_view.hpp"
#include "tab/media_movie.hpp"
#include "view/h_recycling.hpp"
#include "view/video_card.hpp"
#include "view/text_box.hpp"
#include "view/people_source.hpp"
#include "view/video_source.hpp"
#include "view/mpv_core.hpp"
#include "view/icon_button.hpp"
#include "api/jellyfin.hpp"
#include "api/fntv.hpp"
#include "utils/misc.hpp"
#include "utils/dialog.hpp"
#include <fmt/ranges.h>

using namespace brls::literals;  // for _i18n

MediaMovie::MediaMovie(const jellyfin::Item& item) : itemId(item.Id), currentItem(item) {
    brls::Logger::debug("Tab MediaMovie: create");
    // Inflate the tab from the XML file
    this->inflateFromXMLRes("xml/tabs/movie.xml");

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
    this->people->registerCell("Cell", MediaCardCell::create);
    this->similar->registerCell("Cell", VideoCardCell::create);

    if (brls::Application::getThemeVariant() == brls::ThemeVariant::LIGHT) {
        this->imageFade->setImageFromRes("img/fade-bottom-light.png");
    }
    // the buttons and the cast row have no geometric overlap: D-pad nav
    // cannot find it. Explicit route — the row materializes its first cell
    // if needed (HRecyclerFrame::getDefaultFocus) and the "centered" scroll
    // follows the focus
    this->btnPlay->setCustomNavigationRoute(brls::FocusDirection::DOWN, "movie/people");
    this->btnDownload->setCustomNavigationRoute(brls::FocusDirection::DOWN, "movie/people");

    this->btnPlay->registerClickAction([this](...) {
        PlayerView* view = new PlayerView(this->currentItem);
        view->setTitie(this->currentItem.Name);
        return true;
    });

    this->btnRestart->registerClickAction([this](...) {
        PlayerView* view = new PlayerView(this->currentItem, 0);
        view->setTitie(this->currentItem.Name);
        return true;
    });

    auto& dm = DownloadManager::instance();
    this->updateDownloadButton();
    // live progress on the button (events emitted on the UI thread)
    this->progressSub =
        dm.getProgressEvent()->subscribe([this](const std::string& id, int64_t downloaded, int64_t total) {
            if (id != this->itemId || total <= 0) return;
            // "Downloading... (42%)" — a bare percentage does not say what
            // the button does; completion goes back through updateDownloadButton
            this->btnDownload->setText(
                fmt::format("{} ({:.0f}%)", "main/download/downloading"_i18n, downloaded * 100.0 / total));
        });
    this->statusSub = dm.getStatusEvent()->subscribe([this](const std::string& id, DownloadStatus status) {
        if (id == this->itemId) this->updateDownloadButton();
    });
    this->btnDownload->registerClickAction([this](...) {
        auto& dm = DownloadManager::instance();
        switch (dm.findItem(this->itemId)) {
        case DownloadStatus::Queued:
        case DownloadStatus::Downloading:
            Dialog::cancelable("main/download/confirm_cancel"_i18n, [this]() {
                DownloadManager::instance().cancelDownload(this->itemId);
                this->updateDownloadButton();
            });
            break;
        case DownloadStatus::Completed:
            brls::Application::notify("main/download/completed"_i18n);
            break;
        default:
            int qi = AppConfig::instance().getValueIndex(AppConfig::DOWNLOAD_QUALITY);
            dm.addDownload(this->itemId, static_cast<DownloadQuality>(qi));
            this->updateDownloadButton();
        }
        return true;
    });

    this->updateFavoriteButton(item.UserData.IsFavorite);
    this->btnFavorite->registerClickAction([this](...) {
        if (this->isFavorite)
            this->unFavorite();
        else
            this->doFavorite();
        return true;
    });

    this->doMovie();
    this->doSimilar();
}

MediaMovie::~MediaMovie() {
    brls::Logger::debug("Tab MediaMovie: delete");
    auto& dm = DownloadManager::instance();
    dm.getProgressEvent()->unsubscribe(this->progressSub);
    dm.getStatusEvent()->unsubscribe(this->statusSub);
    Image::cancel(this->imageLogo);
    Image::cancel(this->imagePoster);
    Image::cancel(this->imageBackdrop);
}

void MediaMovie::updateDownloadButton() {
    auto& dm = DownloadManager::instance();
    switch (dm.findItem(this->itemId)) {
    case DownloadStatus::Completed:
        this->btnDownload->setText("main/download/completed"_i18n);
        break;
    case DownloadStatus::Queued:
    case DownloadStatus::Downloading:
        this->btnDownload->setText("main/download/downloading"_i18n);
        break;
    default:
        this->btnDownload->setText("main/download/start"_i18n);
    }
}

void MediaMovie::doRequest() { this->doMovie(); }

void MediaMovie::doMovie() {
    const auto id = this->itemId;
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
            if (r.Genres.empty()) {
                this->labelGenres->setVisibility(brls::Visibility::GONE);
            } else {
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
            this->btnSource->setVisibility(brls::Visibility::GONE);
            this->btnDownload->setVisibility(brls::Visibility::GONE);
            this->currentItem = r;
            this->playTicks = r.UserData.PlaybackPositionTicks;
            this->btnRestart->setVisibility(this->playTicks > 0 ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
            this->labelStatus->setText(r.RunTimeTicks > 0 ? fmt::format("时长 {}", misc::sec2Time(r.RunTimeTicks / jellyfin::PLAYTICKS)) : "");
            const std::string target = r.People.empty() ? "movie/label/overview" : "movie/people";
            this->btnPlay->setCustomNavigationRoute(brls::FocusDirection::DOWN, target);
            this->btnRestart->setCustomNavigationRoute(brls::FocusDirection::DOWN, target);
            this->btnFavorite->setCustomNavigationRoute(brls::FocusDirection::DOWN, target);
            this->btnPlay->setText(
                this->playTicks > 0 ? "继续播放 " + misc::sec2Time(this->playTicks / jellyfin::PLAYTICKS) : "main/media/play"_i18n);
        },
        [ASYNC_TOKEN](const std::string& ex) {
            ASYNC_RELEASE
            this->labelPeople->setVisibility(brls::Visibility::GONE);
            this->people->setVisibility(brls::Visibility::GONE);
            this->labelStatus->setText("详情加载失败，按 Y 重试");
            this->labelOverview->setText(ex);
            brls::Logger::warning("doMovie {}", ex);
        });
}

void MediaMovie::doSimilar() {
    this->similar->setVisibility(brls::Visibility::GONE);
    this->labelSimilar->setVisibility(brls::Visibility::GONE);
}


bool MediaMovie::doFavorite() {
    const auto id = this->itemId;
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

bool MediaMovie::unFavorite() { return this->doFavorite(); }

void MediaMovie::updateFavoriteButton(bool favorite) {
    this->isFavorite = favorite;
    if (favorite) {
        this->btnFavorite->setIcon("icon/ico-heart.svg");
        this->btnFavorite->setText("main/media/del_favorite"_i18n);
    } else {
        this->btnFavorite->setIcon("icon/ico-heart-gray.svg");
        this->btnFavorite->setText("main/media/add_favorite"_i18n);
    }
}
