/*
    Copyright 2023 dragonflylee
*/

#include "tab/home_tab.hpp"
#include "view/recyling_video.hpp"
#include "api/fntv.hpp"
#include "utils/keybind.hpp"

using namespace brls::literals;

HomeTab::HomeTab() {
    brls::Logger::debug("Tab HomeTab: create");
    this->inflateFromXMLRes("xml/tabs/home.xml");
    this->userResume->setVisibility(brls::Visibility::GONE);
    this->showNextup->setVisibility(brls::Visibility::GONE);
}

HomeTab::~HomeTab() { brls::Logger::debug("View HomeTab: delete"); }

brls::View* HomeTab::create() { return new HomeTab(); }

void HomeTab::doRequest() {
    for (auto recyler : this->latest) recyler->doRequest(true);
}

void HomeTab::onCreate() {
    auto actionRefresh = [this](brls::View* view) {
        this->doRequest();
        return true;
    };
    this->registerAction("hints/refresh"_i18n, brls::BUTTON_BACK, actionRefresh);
    this->registerAction(KeyBind::getRefresh(), actionRefresh);

    ASYNC_RETAIN
    fntv::async<std::vector<jellyfin::Collection>>(
        [] { return fntv::listLibraries(); },
        [ASYNC_TOKEN](const std::vector<jellyfin::Collection>& libs) {
            ASYNC_RELEASE
            for (auto recyler : this->latest) {
                this->boxHome->removeView(recyler, false);
                recyler->setParent(nullptr);
            }
            this->latest.clear();
            for (auto& item : libs) {
                if (item.CollectionType == "livetv" || item.CollectionType == "music") continue;
                RecylingVideo* recyler = new RecylingVideo();
                recyler->setTitle(item.Name);
                recyler->setFrameHeight(300);
                recyler->setItemWidth(175);
                recyler->setPageSize(12);
                std::string itemId = item.Id;
                std::string type;
                if (item.CollectionType == "movies") type = jellyfin::mediaTypeMovie;
                else if (item.CollectionType == "tvshows") type = jellyfin::mediaTypeSeries;
                recyler->onFetch([itemId, type](size_t start, size_t pageSize) {
                    return fntv::listItems(itemId, start, pageSize, type);
                });
                recyler->doRequest();
                this->latest.push_back(recyler);
                this->boxHome->addView(recyler);
            }
        },
        [ASYNC_TOKEN](const std::string& ex) {
            ASYNC_RELEASE
            brls::Logger::warning("HomeTab {}", ex);
        });
}
