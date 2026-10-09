#include "activity/main_activity.hpp"
#include "utils/config.hpp"

MainActivity::MainActivity() {
    brls::Logger::debug("MainActivity: create");

    auto& conf = AppConfig::instance();
    conf.checkDanmuku();

    // The Jellyfin remote-control websocket (/socket?api_key=...) does not exist on Feiniu.
    // Opening it put the session token into a URL, reconnected forever with a growing
    // back-off, and its destructor joined a thread that could be sleeping for minutes
    // (UI freeze on logout or server switch). It stays disabled for this client.
}
