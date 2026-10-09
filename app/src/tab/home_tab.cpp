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
    this->userResume->onFetch([](size_t start, size_t pageSize) { return fntv::listResume(start, pageSize); });
    // An empty "continue watching" row is noise on a fresh account: show it only with content.
    this->userResume->setHideWhenEmpty(true);
    this->showNextup->setVisibility(brls::Visibility::GONE);
}

HomeTab::~HomeTab() { brls::Logger::debug("View HomeTab: delete"); }

brls::View* HomeTab::create() { return new HomeTab(); }

void HomeTab::doRequest() {
    this->userResume->doRequest(true);
    // The library list itself failed earlier (NAS asleep, Wi-Fi not ready): retry it too,
    // otherwise the home page stayed empty until the app was restarted.
    if (this->latest.empty()) {
        this->loadLibraries();
        return;
    }
    for (auto recyler : this->latest) recyler->doRequest(true);
}

void HomeTab::onCreate() {
    this->userResume->doRequest(true);
    auto actionRefresh = [this](brls::View* view) {
        this->doRequest();
        return true;
    };
    this->registerAction("hints/refresh"_i18n, brls::BUTTON_BACK, actionRefresh);
    this->registerAction(KeyBind::getRefresh(), actionRefresh);

    this->loadLibraries();
}

void HomeTab::loadLibraries() {
    if (this->librariesLoading) return;
    this->librariesLoading = true;
    ASYNC_RETAIN
    fntv::async<std::vector<jellyfin::Collection>>(
        [] { return fntv::listLibraries(); },
        [ASYNC_TOKEN](const std::vector<jellyfin::Collection>& libs) {
            ASYNC_RELEASE
            this->librariesLoading = false;
            for (auto recyler : this->latest) {
                this->boxHome->removeView(recyler, false);
                recyler->setParent(nullptr);
            }
            this->latest.clear();
            for (auto& item : libs) {
                if (item.CollectionType == "livetv" || item.CollectionType == "music") continue;
                RecylingVideo* recyler = new RecylingVideo();
                // Newest additions first, like the Feiniu home page (name order told nothing new).
                recyler->setTitle(item.Name + " · 最近添加");
                recyler->setFrameHeight(300);
                recyler->setItemWidth(175);
                recyler->setPageSize(12);
                std::string itemId = item.Id;
                recyler->onFetch([itemId](size_t start, size_t pageSize) {
                    return fntv::listItems(itemId, start, pageSize, "", fntv::SORT_ADDED, "DESC");
                });
                recyler->doRequest();
                this->latest.push_back(recyler);
                this->boxHome->addView(recyler);
            }
            if (libs.empty()) brls::Application::notify("NAS 上还没有媒体库，请先在飞牛影视中添加");
        },
        [ASYNC_TOKEN](const std::string& ex) {
            ASYNC_RELEASE
            this->librariesLoading = false;
            brls::Logger::warning("HomeTab {}", ex);
            brls::Application::notify("媒体库加载失败：" + ex + "（按 - 刷新）");
        });
}
