#include "view/context_menu.hpp"
#include "view/svg_image.hpp"
#include "view/mpv_core.hpp"
#include "view/video_card.hpp"
#include "api/jellyfin.hpp"
#include "api/fntv.hpp"
#include "utils/dialog.hpp"
#include "utils/download.hpp"

using namespace brls::literals;

const std::string menuItemXML = R"xml(
    <brls:Box
        height="50"
        axis="row"
        focusable="true"
        cornerRadius="10"
        highlightCornerRadius="10"
        alignItems="center"
        paddingLeft="14"
        paddingRight="14">

        <SVGImage
            id="menu_item/icon"
            width="20"
            height="20"
            marginRight="16" />

        <brls:Label
            id="menu_item/title"
            fontSize="16"
            grow="1.0" />
    </brls:Box>
)xml";

MenuItem::MenuItem() {
    this->inflateFromXMLString(menuItemXML);

    this->registerStringXMLAttribute("icon", [this](std::string value) { this->setIcon(value); });
    this->registerStringXMLAttribute("title", [this](std::string value) { this->setTitle(value); });
}

void MenuItem::setIcon(const std::string& res) {
    std::string path = res;
    const std::string prefix = "@res/";
    if (path.rfind(prefix, 0) == 0) path = path.substr(prefix.size());
    this->icon->setImageFromSVGRes(path);
}

void MenuItem::setTitle(const std::string& text) { this->title->setText(text); }

brls::View* MenuItem::create() { return new MenuItem(); }

ContextMenu::ContextMenu(const jellyfin::Episode& item, BaseCardCell* view) : item(item), itemId(item.Id), cell(view) {
    this->inflateFromXMLRes("xml/view/context_menu.xml");
    brls::Logger::debug("ContextMenu: create");

    this->registerAction("hints/cancel"_i18n, brls::BUTTON_B, [](brls::View* view) {
        brls::Application::popActivity();
        return true;
    });
    this->cancel->registerClickAction([](brls::View* view) {
        brls::Application::popActivity();
        return true;
    });
    this->cancel->addGestureRecognizer(new brls::TapGestureRecognizer(this->cancel));

    this->labelTitle->setText(item.Type == jellyfin::mediaTypeEpisode ? fntv::episodeTitle(item) : item.Name);

    this->btnFavorite->registerClickAction([this](brls::View* view) {
        if (this->isFavorite)
            return this->unFavorite();
        else
            return this->doFavorite();
    });
    this->btnFavorite->addGestureRecognizer(new brls::TapGestureRecognizer(this->btnFavorite));
    this->updateFavoriteButton(item.UserData.IsFavorite);

    this->btnMarkPlay->registerClickAction([this](brls::View* view) {
        if (this->isPlayed)
            return this->unPlayed();
        else
            return this->doPlayed();
    });
    this->btnMarkPlay->addGestureRecognizer(new brls::TapGestureRecognizer(this->btnMarkPlay));
    this->updatePlayedButton(item.UserData.Played);
    // A directory has no watched state of its own.
    if (item.Type == jellyfin::mediaTypeFolder) this->btnMarkPlay->setVisibility(brls::Visibility::GONE);

    // Downloads use Jellyfin endpoints and are not available for Feiniu.
    this->btnDownload->setVisibility(brls::Visibility::GONE);
}

ContextMenu::~ContextMenu() {}

bool ContextMenu::doPlayed() {
    this->setPlayed(true);
    return true;
}

bool ContextMenu::unPlayed() {
    this->setPlayed(false);
    return true;
}

void ContextMenu::setPlayed(bool played) {
    const auto target = this->item;
    ASYNC_RETAIN
    fntv::async<bool>(
        [target, played] {
            bool synced = true;
            try {
                fntv::setWatched(target.Id, played);
            } catch (const fntv::ApiError& ex) {
                // Offline or logged out: nothing was changed, report it.
                if (ex.kind == fntv::ErrorKind::Network || ex.kind == fntv::ErrorKind::Auth) throw;
                // This NAS version has no watched endpoint: keep the mark on this Switch.
                synced = false;
            }
            fntv::markLocalWatched(target, played);
            return synced;
        },
        [ASYNC_TOKEN, played](bool synced) {
            ASYNC_RELEASE
            this->item.UserData.Played = played;
            this->updatePlayedButton(played, true);
            if (!synced) brls::Application::notify("已在本机标记，当前 NAS 版本不支持同步观看状态");
        },
        [ASYNC_TOKEN](const std::string& ex) {
            ASYNC_RELEASE
            brls::Application::notify(ex);
        });
}

bool ContextMenu::doFavorite() {
    const auto id = this->itemId;
    const bool favorite = !this->isFavorite;
    ASYNC_RETAIN
    fntv::async<bool>(
        [id, favorite] { return fntv::setFavorite(id, favorite); },
        [ASYNC_TOKEN](bool favorite) {
            ASYNC_RELEASE
            this->updateFavoriteButton(favorite, true);
        },
        [ASYNC_TOKEN](const std::string& ex) {
            ASYNC_RELEASE
            brls::Application::notify(ex);
        });
    return true;
}

bool ContextMenu::unFavorite() { return this->doFavorite(); }

void ContextMenu::updatePlayedButton(bool played, bool applyToCell) {
    // Only reflect real changes on the card: opening the menu must not hide a progress bar.
    if (applyToCell && this->cell) this->cell->setWatched(played);
    this->isPlayed = played;
    if (played) {
        this->btnMarkPlay->setIcon("icon/ico-checkmark.svg");
        this->btnMarkPlay->setTitle("main/media/mark_unplayed"_i18n);
    } else {
        this->btnMarkPlay->setIcon("icon/ico-unchecked.svg");
        this->btnMarkPlay->setTitle("main/media/mark_played"_i18n);
    }
}

void ContextMenu::updateFavoriteButton(bool favorite, bool applyToCell) {
    if (applyToCell && this->cell) this->cell->setFavorite(favorite);
    this->isFavorite = favorite;
    if (favorite) {
        this->btnFavorite->setIcon("icon/ico-heart.svg");
        this->btnFavorite->setTitle("main/media/del_favorite"_i18n);
    } else {
        this->btnFavorite->setIcon("icon/ico-heart-gray.svg");
        this->btnFavorite->setTitle("main/media/add_favorite"_i18n);
    }
}

void ContextMenu::updateDownloadButton() {
    auto& dm = DownloadManager::instance();
    switch (dm.findItem(this->itemId)) {
    case DownloadStatus::Completed:
        this->btnDownload->setTitle("main/download/completed"_i18n);
        break;
    case DownloadStatus::Queued:
    case DownloadStatus::Downloading:
        this->btnDownload->setTitle("main/download/downloading"_i18n);
        break;
    default:
        this->btnDownload->setTitle("main/download/start"_i18n);
    }
}