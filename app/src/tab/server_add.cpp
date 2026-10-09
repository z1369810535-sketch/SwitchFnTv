/*
    Copyright 2023 dragonflylee
*/

#include "tab/server_add.hpp"
#include "tab/server_login.hpp"
#include "utils/config.hpp"
#include "utils/dialog.hpp"
#include "api/fntv.hpp"

using namespace brls::literals;  // for _i18n

ServerAdd::ServerAdd() {
    // Inflate the tab from the XML file
    this->inflateFromXMLRes("xml/tabs/server_add.xml");
    brls::Logger::debug("ServerAdd: create");

    inputUrl->init("main/setting/url"_i18n, fntv::DEFAULT_BASE_URL, [](std::string) {}, "http://NAS地址:5666", "", 255);

    btnConnect->registerClickAction([this](...) { return this->onConnect(); });
}

ServerAdd::~ServerAdd() { brls::Logger::debug("ServerAdd Activity: delete"); }

brls::View* ServerAdd::getDefaultFocus() { return this->inputUrl; }

bool ServerAdd::onConnect() {
    brls::Application::blockInputs();
    std::string baseUrl = fntv::normalizeBaseUrl(this->inputUrl->getValue());
    if (baseUrl.length() < 10 || baseUrl.substr(0, 4).compare("http")) {
        brls::Application::unblockInputs();
        Dialog::show("main/setting/server/invalid"_i18n);
        return false;
    }
    this->btnConnect->setTextColor(brls::Application::getTheme().getColor("font/grey"));
    brls::Logger::debug("ServerAdd onConnect: click {}", baseUrl);

    this->btnConnect->setDetailText("正在连接…");

    ASYNC_RETAIN
    brls::async([ASYNC_TOKEN, baseUrl]() {
        try {
            // Inputs are blocked meanwhile: probe quickly instead of 3 x 15 s.
            auto cfg = fntv::sysConfig(baseUrl, 5000, 2);
            if (!cfg.is_object()) throw std::runtime_error("该地址不是飞牛影视服务（默认端口 5666）");
            AppServer s = {
                .name = fntv::serverName(cfg),
                .id = fntv::md5Hex(baseUrl),
                .urls = {baseUrl},
            };
            brls::sync([ASYNC_TOKEN, s]() {
                ASYNC_RELEASE
                this->btnConnect->setDetailText("");
                this->btnConnect->setTextColor(brls::Application::getTheme().getColor("brls/text"));
                brls::Application::unblockInputs();
                if (AppConfig::instance().addServer(s)) {
                    brls::Application::popActivity(brls::TransitionAnimation::NONE);
                } else {
                    this->present(new ServerLogin(s.name, s.urls.front()));
                }
            });
        } catch (const std::exception& ex) {
            std::string msg = ex.what();
            // Something answered, but not the Feiniu API (wrong port, router page, another service).
            if (auto api = dynamic_cast<const fntv::ApiError*>(&ex)) {
                if (api->kind == fntv::ErrorKind::Format || api->code == 404)
                    msg = "该地址不是飞牛影视服务，请确认端口（默认 5666）";
            }
            brls::sync([ASYNC_TOKEN, msg]() {
                ASYNC_RELEASE
                this->btnConnect->setDetailText("");
                this->btnConnect->setTextColor(brls::Application::getTheme().getColor("brls/text"));
                brls::Application::unblockInputs();
                Dialog::show(msg.empty() ? "连接失败" : msg);
            });
        } catch (...) {
            brls::sync([ASYNC_TOKEN]() {
                ASYNC_RELEASE
                this->btnConnect->setDetailText("");
                this->btnConnect->setTextColor(brls::Application::getTheme().getColor("brls/text"));
                brls::Application::unblockInputs();
                Dialog::show("连接失败");
            });
        }
    });
    return false;
}
