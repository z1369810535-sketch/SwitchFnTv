#include "activity/player_view.hpp"
#include "api/jellyfin.hpp"
#include "api/fntv.hpp"
#include "utils/dialog.hpp"
#include "utils/misc.hpp"
#include "view/danmaku_core.hpp"
#include "view/mpv_core.hpp"
#include "view/player_setting.hpp"
#include "view/video_view.hpp"
#include "view/video_profile.hpp"
#include <tinyxml2.h>
#include <sstream>

using namespace brls::literals;

PlayerView::PlayerView(const jellyfin::Item& item, const uint64_t seekTicks, const std::string& sourceId)
    : itemId(item.Id) {
    float width = brls::Application::contentWidth;
    float height = brls::Application::contentHeight;
    view = new VideoView();
    view->setDimensions(width, height);
    view->setWidthPercentage(100);
    view->setHeightPercentage(100);
    view->setId("video");
    this->setDimensions(width, height);
    this->addView(view);
    view->registerVideoQuality([this](...) { return this->toggleQuality(); });

    auto& mpv = MPVCore::instance();

    brls::Application::pushActivity(new brls::Activity(this), brls::TransitionAnimation::NONE);

    playSubscribeID = view->getPlayEvent()->subscribe([this](int index) { this->playIndex(index); });

    settingSubscribeID = view->getSettingEvent()->subscribe([this]() {
        brls::View* setting = new PlayerSetting(&this->stream);
        brls::Application::pushActivity(new brls::Activity(setting));
    });

    eventSubscribeID = mpv.getEvent()->subscribe([this](MpvEventEnum event) {
        auto& mpv = MPVCore::instance();
        // brls::Logger::info("mpv event => : {}", event);
        switch (event) {
        case MpvEventEnum::MPV_RESUME:
            this->reportPlay();
            view->getProfile()->init(this->playMethod);
            break;
        case MpvEventEnum::MPV_PAUSE:
            this->reportPlay(true);
            break;
        case MpvEventEnum::LOADING_END:
            this->reportStart();
            break;
        case MpvEventEnum::MPV_STOP:
            this->reportStop();
            break;
        case MpvEventEnum::MPV_LOADED: {
            const char* flag = MPVCore::SUBS_FALLBACK ? "select" : "auto";
            for (auto& s : this->stream.MediaStreams) {
                if (s.Type == jellyfin::streamTypeSubtitle && s.IsExternal && !s.DeliveryUrl.empty()) {
                    mpv.command("sub-add", s.DeliveryUrl.c_str(), flag, s.DisplayTitle.c_str());
                }
            }
            if (PlayerSetting::selectedSubtitle > 0 && this->playMethod == jellyfin::methodDirectPlay) {
                mpv.setInt("sid", PlayerSetting::selectedSubtitle);
            }
            break;
        }
        case MpvEventEnum::UPDATE_PROGRESS:
            if (mpv.video_progress % 10 == 0) this->reportPlay();
            break;
        default:;
        }
    });
    // 自定义的mpv事件
    customEventSubscribeID = mpv.getCustomEvent()->subscribe([this](const std::string& event, void* data) {
        if (event == QUALITY_CHANGE) {
            this->playMedia(MPVCore::instance().playback_time * jellyfin::PLAYTICKS);
        } else if (event == SYNC_STOP) {
            VideoView::close();
        } else if (event == "PreviousTrack") {
            this->view->playNext(-1);
        } else if (event == "NextTrack") {
            this->view->playNext(1);
        }
    });

    if (item.Type != jellyfin::mediaTypeTvChannel) {
        this->sourceId = sourceId.empty() ? item.Id : sourceId;
    }

    this->setChapters(item.Chapters, item.RunTimeTicks);
    this->playMedia(seekTicks > 0 ? seekTicks : item.UserData.PlaybackPositionTicks);

    // Report stop when application exit
    this->exitSubscribeID = brls::Application::getExitEvent()->subscribe([this]() {
        if (!MPVCore::instance().isStopped()) this->reportStop();
    });
}

PlayerView::~PlayerView() {
    auto& mpv = MPVCore::instance();
    mpv.getEvent()->unsubscribe(eventSubscribeID);
    mpv.getCustomEvent()->unsubscribe(customEventSubscribeID);
    view->getPlayEvent()->unsubscribe(playSubscribeID);
    view->getSettingEvent()->unsubscribe(settingSubscribeID);

    brls::sync([&mpv]() { mpv.getCustomEvent()->fire(VIDEO_CLOSE, nullptr); });

    if (DanmakuCore::PLUGIN_ACTIVE) {
        DanmakuCore::instance().reset();
    }

    PlayerSetting::selectedSubtitle = 0;
    PlayerSetting::selectedAudio = 0;

    if (!mpv.isStopped()) this->reportStop();
    brls::Application::getExitEvent()->unsubscribe(this->exitSubscribeID);
    fntv::cleanupSubtitles();
    brls::Logger::debug("trying delete PlayerView...");
}

void PlayerView::setSeries(const std::string& seriesId) {
    ASYNC_RETAIN
    fntv::async<jellyfin::Result<jellyfin::Episode>>(
        [seriesId] { return fntv::listEpisodes(seriesId); },
        [ASYNC_TOKEN](const jellyfin::Result<jellyfin::Episode>& r) {
            ASYNC_RELEASE
            int index = -1;
            std::vector<std::string> values;
            for (size_t i = 0; i < r.Items.size(); i++) {
                auto& item = r.Items.at(i);
                if (item.Id == this->itemId) index = static_cast<int>(i);
                values.push_back(fmt::format("S{}E{} - {}", item.ParentIndexNumber, item.IndexNumber, item.Name));
            }
            view->setList(values, index);
            this->episodes = std::move(r.Items);
        },
        [ASYNC_TOKEN](const std::string& error) {
            ASYNC_RELEASE
            brls::Logger::warning("setSeries {}", error);
        });
}

void PlayerView::setTitie(const std::string& title) { this->view->setTitie(title); }

void PlayerView::setChapters(const std::vector<jellyfin::MediaChapter>& chaps, uint64_t duration) {
    this->view->setChapters(chaps, duration);
}

bool PlayerView::playIndex(int index) {
    if (index < 0 || index >= (int)this->episodes.size()) {
        return VideoView::close();
    }
    MPVCore::instance().reset();

    auto item = this->episodes.at(index);
    this->itemId = item.Id;
    this->sourceId = item.Id;
    this->setChapters(item.Chapters, item.RunTimeTicks);
    this->playMedia(0);
    view->setTitie(fmt::format("S{}E{} - {}", item.ParentIndexNumber, item.IndexNumber, item.Name));
    return true;
}

void PlayerView::playMedia(const uint64_t seekTicks) {
    ASYNC_RETAIN
    fntv::async<fntv::PlaySession>(
        [this] { return fntv::preparePlay(this->itemId); },
        [ASYNC_TOKEN, seekTicks](const fntv::PlaySession& session) {
            ASYNC_RELEASE
            auto& mpv = MPVCore::instance();
            this->stream = session.source;
            this->sourceId = session.media_guid;
            this->playMethod = jellyfin::methodDirectPlay;
            this->playSessionId = session.media_guid;

            std::stringstream ssextra;
            ssextra << fmt::format("network-timeout={}", HTTP::TIMEOUT / 100);
            if (seekTicks > 0) ssextra << ",start=" << misc::sec2Time(seekTicks / jellyfin::PLAYTICKS);
            const std::string token = AppConfig::instance().getToken();
            std::string headers = "Authorization: " + token;
            if (!session.cookie_header.empty()) headers += "\r\nCookie: " + session.cookie_header;
            ssextra << ",http-header-fields=\"" << headers << "\"";
            if (HTTP::PROXY_STATUS) ssextra << ",http-proxy=\"" << HTTP::PROXY << "\"";

            std::string url = session.direct_url;
            if (url.empty()) url = session.fallback_url;
            if (url.empty()) {
                Dialog::show("?????????", []() { VideoView::close(); });
                return;
            }
            this->stream.TranscodingUrl = session.fallback_url;
            mpv.setUrl(url, ssextra.str());
        },
        [ASYNC_TOKEN](const std::string& ex) {
            ASYNC_RELEASE
            Dialog::show(ex, []() { VideoView::close(); });
        });
}

void PlayerView::reportStart() {}

void PlayerView::reportStop() {}

void PlayerView::reportPlay(bool isPaused) { (void)isPaused; }

void PlayerView::requestDanmaku() {
    ASYNC_RETAIN
    brls::async([ASYNC_TOKEN]() {
        auto& c = AppConfig::instance();
        HTTP::Header header = {"X-Emby-Token: " + c.getToken()};
        std::string url = fmt::format(fmt::runtime(jellyfin::apiDanmuku), this->itemId);

        try {
            std::string resp = HTTP::get(c.getUrl() + url, header, HTTP::Timeout{});

            ASYNC_RELEASE
            brls::Logger::debug("DANMAKU: start decode");

            // Load XML
            tinyxml2::XMLDocument document = tinyxml2::XMLDocument();
            tinyxml2::XMLError error = document.Parse(resp.c_str());

            if (error != tinyxml2::XMLError::XML_SUCCESS) {
                brls::Logger::error("Parse danmaku xml[1]: {}", std::to_string(error));
                return;
            }
            tinyxml2::XMLElement* element = document.RootElement();
            if (!element) {
                brls::Logger::error("Decode danmaku xml[2]: no root element");
                return;
            }

            std::vector<DanmakuItem> items;
            for (auto child = element->FirstChildElement(); child != nullptr; child = child->NextSiblingElement()) {
                if (strcmp(child->Name(), "d")) continue;  // 简易判断是不是弹幕
                const char* content = child->GetText();
                if (!content) continue;
                try {
                    items.emplace_back(content, child->Attribute("p"));
                } catch (...) {
                    brls::Logger::error("DANMAKU: error decode: {}", child->GetText());
                }
            }

            brls::sync([items, this]() {
                DanmakuCore::instance().loadDanmakuData(items);
                view->setDanmakuEnable(brls::Visibility::VISIBLE);
            });

            brls::Logger::debug("DANMAKU: decode done: {}", items.size());

        } catch (const std::exception& ex) {
            ASYNC_RELEASE
            brls::Logger::warning("request danmu: {}", ex.what());

            brls::sync([this]() {
                DanmakuCore::instance().reset();
                view->setDanmakuEnable(brls::Visibility::GONE);
            });
        }
    });
}

bool PlayerView::toggleQuality() {
    static std::set<std::string> codecs = {"hevc", "av1", "vp9"};
    std::vector<std::string> options = {"main/player/auto"_i18n};
    std::vector<int64_t> values = {0};
    int64_t videoBitRate = this->stream.Bitrate;
    if (videoBitRate <= 20000000) {
        for (const auto& stream : this->stream.MediaStreams) {
            if (stream.Type == "Video" && codecs.count(stream.Codec) > 0) {
                videoBitRate = round(videoBitRate * 1.5);
                break;
            }
        }
    }

    if (videoBitRate >= 15000000) options.push_back("20 Mbps"), values.push_back(20000000);
    if (videoBitRate >= 10000000) options.push_back("15 Mbps"), values.push_back(15000000);
    if (videoBitRate >= 8000000) options.push_back("10 Mbps"), values.push_back(10000000);
    if (videoBitRate >= 6000000) options.push_back("8 Mbps"), values.push_back(8000000);
    if (videoBitRate >= 4000000) options.push_back("6 Mbps"), values.push_back(6000000);
    if (videoBitRate >= 3000000) options.push_back("4 Mbps"), values.push_back(4000000);
    if (videoBitRate >= 1500000) options.push_back("3 Mbps"), values.push_back(3000000);
    if (videoBitRate >= 720000) options.push_back("1.5 Mbps"), values.push_back(1500000);
    options.push_back("720 kbps"), values.push_back(720000);
    options.push_back("420 kbps"), values.push_back(420000);

    auto it = std::find(values.begin(), values.end(), MPVCore::VIDEO_QUALITY);
    if (it == values.end()) it = values.begin();

    brls::Dropdown* dropdown = new brls::Dropdown(
        "main/player/quality"_i18n, options,
        [values](int selected) {
            MPVCore::VIDEO_QUALITY = values[selected];
            MPVCore::instance().getCustomEvent()->fire(QUALITY_CHANGE, nullptr);
            return true;
        },
        std::distance(values.begin(), it));

    brls::Application::pushActivity(new brls::Activity(dropdown));
    return true;
}
