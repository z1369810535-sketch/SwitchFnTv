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

    inputUrl->init("URL", fntv::DEFAULT_BASE_URL, [](std::string) {}, fntv::DEFAULT_BASE_URL, "", 255);

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

    ASYNC_RETAIN
    brls::async([ASYNC_TOKEN, baseUrl]() {
        try {
            auto cfg = fntv::sysConfig(baseUrl);
            auto info = fntv::inspectSysConfig(cfg);
            std::string name = "飞牛影视";
            if (cfg.is_object()) {
                if (cfg.contains("name") && cfg["name"].is_string() && !cfg["name"].get<std::string>().empty())
                    name = cfg["name"].get<std::string>();
            }
            AppServer s = {
                .name = name,
                .id = fntv::md5Hex(baseUrl),
                .urls = {baseUrl},
            };
            (void)info;
            brls::sync([ASYNC_TOKEN, s]() {
                ASYNC_RELEASE
                brls::Application::unblockInputs();
                if (AppConfig::instance().addServer(s)) {
                    brls::Application::popActivity(brls::TransitionAnimation::NONE);
                } else {
                    this->present(new ServerLogin(s.name, s.urls.front()));
                }
            });
        } catch (const std::exception& ex) {
            std::string msg = ex.what();
            brls::sync([ASYNC_TOKEN, msg]() {
                ASYNC_RELEASE
                this->btnConnect->setTextColor(brls::Application::getTheme().getColor("brls/text"));
                brls::Application::unblockInputs();
                Dialog::show(msg.empty() ? "Connect failed" : msg);
            });
        } catch (...) {
            brls::sync([ASYNC_TOKEN]() {
                ASYNC_RELEASE
                this->btnConnect->setTextColor(brls::Application::getTheme().getColor("brls/text"));
                brls::Application::unblockInputs();
                Dialog::show("Connect failed");
            });
        }
    });
    return false;
}
