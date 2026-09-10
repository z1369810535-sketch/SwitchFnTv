/*
    Copyright 2023 dragonflylee
*/

#include "tab/server_login.hpp"
#include "activity/main_activity.hpp"
#include "api/fntv.hpp"
#include "api/analytics.hpp"
#include "utils/dialog.hpp"

using namespace brls::literals;  // for _i18n

ServerLogin::ServerLogin(const std::string& name, const std::string& url, const std::string& user) : url(url) {
    this->inflateFromXMLRes("xml/tabs/server_login.xml");
    brls::Logger::debug("ServerLogin: create {}", url);

    this->hdrSigin->setTitle(brls::getStr("main/setting/server/sigin_to", name));
    this->inputUser->init("main/setting/username"_i18n, user);
    this->inputPass->init("main/setting/password"_i18n, "", [](std::string text) {}, "", "", 256);

    this->btnSignin->registerClickAction([this](...) { return this->onSignin(); });
    this->btnQuickConnect->setVisibility(brls::Visibility::GONE);
    this->labelDisclaimer->setText("Password is not saved. Session token is stored on the SD card and is not secure.");
    this->labelDisclaimer->setVisibility(brls::Visibility::VISIBLE);
}

ServerLogin::~ServerLogin() { brls::Logger::debug("ServerLogin Activity: delete"); }

bool ServerLogin::onSignin() {
    std::string username = inputUser->getValue();
    std::string password = inputPass->getValue();
    if (username.empty()) {
        Dialog::show("Username is empty");
        return false;
    }

    brls::Application::blockInputs();
    this->btnSignin->setState(brls::ButtonState::DISABLED);

    ASYNC_RETAIN
    brls::async([ASYNC_TOKEN, username, password]() {
        try {
            auto r = fntv::login(this->url, username, password);
            AppUser u = {
                .id = r.user_id,
                .name = r.user_name,
                .access_token = r.token,
                .server_id = r.server_id,
            };
            brls::sync([ASYNC_TOKEN, u]() {
                ASYNC_RELEASE
                AppConfig::instance().addUser(u, this->url);
                this->btnSignin->setState(brls::ButtonState::ENABLED);
                brls::Application::unblockInputs();
                brls::Application::clear();
                brls::Application::pushActivity(new MainActivity(), brls::TransitionAnimation::NONE);
                GA("login", {{"method", {this->url}}});
            });
        } catch (const std::exception& ex) {
            std::string msg = ex.what();
            brls::sync([ASYNC_TOKEN, msg]() {
                ASYNC_RELEASE
                this->btnSignin->setState(brls::ButtonState::ENABLED);
                brls::Application::unblockInputs();
                Dialog::show(msg);
            });
        }
    });
    return true;
}

void ServerLogin::Disclaimer() {}

void ServerLogin::doQuickLogin() {}
