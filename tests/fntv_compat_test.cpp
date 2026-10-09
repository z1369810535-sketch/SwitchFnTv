// Error handling, retry policy, sorting and watched-state checks for the Feiniu API layer.
// Like the other tests it compiles the production fntv.cpp with a fake transport.
#include "api/fntv.hpp"
#include "utils/config.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>

using json = nlohmann::json;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

json ok(const json& data) { return {{"code", 0}, {"data", data}}; }

template <typename F>
fntv::ApiError expectError(F&& f, const char* message) {
    try {
        f();
    } catch (const fntv::ApiError& ex) {
        return ex;
    }
    throw std::runtime_error(message);
}

void businessErrorsAreNotRetried() {
    int calls = 0;
    HTTP::respond = [&calls](auto, auto, auto) {
        ++calls;
        return json{{"code", 1}, {"msg", "媒体库不存在"}}.dump();
    };
    auto ex = expectError([] { fntv::requestJson("GET", "/v/api/v1/item/x"); }, "business error must throw");
    check(calls == 1, "a business error is answered once, not retried");
    check(ex.kind == fntv::ErrorKind::Server && std::string(ex.what()) == "媒体库不存在", "server message preserved");
}

void networkRetryBudget() {
    int calls = 0;
    HTTP::respond = [&calls](auto, auto, auto) -> std::string {
        ++calls;
        throw std::runtime_error("Couldn't connect to server");
    };
    auto ex = expectError([] { fntv::requestJson("GET", "/v/api/v1/user/info"); }, "network failure must throw");
    check(calls == 2, "default budget retries a transient failure once");
    check(ex.kind == fntv::ErrorKind::Network, "connection failure is a network error");
    check(std::string(ex.what()).find("无法连接") != std::string::npos, "readable connection message");

    calls = 0;
    expectError([] { fntv::userInfo("http://nas.invalid:5666", "token", 4000, 1); }, "probe failure must throw");
    check(calls == 1, "startup probe gives up after one attempt");

    calls = 0;
    int okAfter = 1;
    HTTP::respond = [&calls, &okAfter](auto, auto, auto) -> std::string {
        if (calls++ < okAfter) throw std::runtime_error("Timeout was reached");
        return ok(json{{"guid", "u"}}).dump();
    };
    check(fntv::userInfo().at("guid") == "u" && calls == 2, "a single timeout recovers on the retry");
}

void transportMessages() {
    const std::string base = "http://192.168.1.2:5666";
    check(fntv::transportError("Timeout was reached", base).kind == fntv::ErrorKind::Network, "timeout kind");
    check(std::string(fntv::transportError("Couldn't resolve host name", base).what()).find(base) != std::string::npos,
        "resolve failure names the address");
    auto auth = fntv::transportError("http status 401", base);
    check(auth.kind == fntv::ErrorKind::Auth && !auth.retryable && auth.code == 401, "HTTP 401 means log in again");
    check(fntv::transportError("http status 403", base).kind == fntv::ErrorKind::Auth, "HTTP 403 is an auth problem");
    auto missing = fntv::transportError("http status 404", base);
    check(missing.kind == fntv::ErrorKind::Server && !missing.retryable, "HTTP 404 is not retried");
    check(fntv::transportError("http status 502", base).retryable, "HTTP 5xx is transient");
    check(!fntv::transportError("SSL connect error", base).retryable, "TLS failure is not retried blindly");
    check(fntv::businessError(-1, "token expired").kind == fntv::ErrorKind::Auth, "expired token detected");
    check(fntv::businessError(403, "library access denied").kind == fntv::ErrorKind::Server,
        "permission message is not mistaken for an expired session");
    check(std::string(fntv::businessError(7, "").what()).find("7") != std::string::npos, "empty message keeps code");
}

void invalidSignHint() {
    int calls = 0;
    HTTP::respond = [&calls](auto, auto, auto) {
        ++calls;
        return json{{"code", 5000}, {"msg", "invalid sign"}}.dump();
    };
    auto ex = expectError([] { fntv::requestJson("GET", "/v/api/v1/user/info"); }, "persistent invalid sign must throw");
    check(calls == 5, "invalid sign retried a bounded number of times");
    check(std::string(ex.what()).find("系统时间") != std::string::npos, "points at the Switch clock");
}

void loginErrors() {
    int calls = 0;
    HTTP::respond = [&calls](auto, auto url, auto) {
        ++calls;
        check(url.find("/v/api/v1/login") != std::string::npos, "login endpoint");
        return json{{"code", 1001}, {"msg", "password error"}}.dump();
    };
    auto ex = expectError([] { fntv::login("http://test.invalid", "user", "bad"); }, "wrong password must throw");
    check(calls == 1, "a rejected password is never re-sent");
    check(ex.kind == fntv::ErrorKind::Auth && std::string(ex.what()) == "登录失败，请检查用户名和密码", "login message");

    HTTP::respond = [](auto, auto, auto) { return json{{"code", 1002}, {"msg", "账号已被锁定 5 分钟"}}.dump(); };
    ex = expectError([] { fntv::login("http://test.invalid", "user", "bad"); }, "locked account must throw");
    check(std::string(ex.what()) == "登录失败：账号已被锁定 5 分钟", "readable server reason kept");

    HTTP::respond = [](auto, auto url, auto) {
        if (url.find("/sys/config") != std::string::npos) return ok({{"server_name", "客厅 NAS"}}).dump();
        return ok({{"token", "abc"}}).dump();
    };
    auto r = fntv::login("http://test.invalid", "user", "pass");
    check(r.token == "abc" && r.server_name == "客厅 NAS", "server_name from sys/config");
    check(fntv::serverName(json{{"server_guid", "x"}}) == "飞牛影视", "server name fallback");
}

void sortAndFallback() {
    std::vector<json> requests;
    HTTP::respond = [&requests](auto, auto, const std::string& body) {
        requests.push_back(json::parse(body));
        return ok({{"total", 1}, {"list", json::array({{{"guid", "m"}, {"type", "Movie"}}})}}).dump();
    };
    fntv::listItems("lib", 0, 12, "", fntv::SORT_ADDED, "desc");
    check(requests.back().at("sort_column") == "create_time" && requests.back().at("sort_type") == "DESC",
        "requested sort is sent");
    fntv::listItems("lib", 0, 12);
    check(requests.back().at("sort_column") == "sort_title" && requests.back().at("sort_type") == "ASC",
        "default stays name order");

    requests.clear();
    HTTP::respond = [&requests](auto, auto, const std::string& body) {
        auto req = json::parse(body);
        requests.push_back(req);
        if (req.at("sort_column") != "sort_title") return json{{"code", 400}, {"msg", "invalid sort column"}}.dump();
        return ok({{"total", 1}, {"list", json::array({{{"guid", "m"}, {"type", "Movie"}}})}}).dump();
    };
    auto r = fntv::listItems("lib", 0, 12, "", "vote_average", "DESC");
    check(r.Items.size() == 1 && requests.size() == 2, "unsupported sort falls back to name order");

    int calls = 0;
    HTTP::respond = [&calls](auto, auto, auto) -> std::string {
        ++calls;
        throw std::runtime_error("Timeout was reached");
    };
    expectError([] { fntv::listItems("lib", 0, 12, "", fntv::SORT_ADDED, "DESC"); }, "offline listing must throw");
    check(calls == 2, "network failure is not mistaken for an unsupported sort");

    const auto& options = fntv::sortOptions();
    check(!options.empty() && options.front().column == fntv::SORT_ADDED, "date added listed first");
}

void watchedState() {
    std::vector<std::string> methods;
    HTTP::respond = [&methods](const auto& method, const auto& url, const auto& body) {
        check(url.find("/v/api/v1/item/watched") != std::string::npos, "watched endpoint");
        check(json::parse(body).at("item_guid") == "ep", "watched payload");
        methods.push_back(method);
        return ok(nullptr).dump();
    };
    check(fntv::setWatched("ep", true) && !fntv::setWatched("ep", false), "watched result");
    check(methods.size() == 2 && methods[0] == "PUT" && methods[1] == "DELETE", "PUT marks, DELETE clears");

    auto item = fntv::mapItem({{"guid", "marked"}, {"title", "Marked"}, {"type", "Episode"}, {"duration", 1200}});
    fntv::markLocalWatched(item, true);
    auto copy = item;
    fntv::applyLocalProgress(copy);
    check(copy.UserData.Played && copy.UserData.PlaybackPositionTicks == 0, "local mark applied");
    HTTP::respond = [](auto, auto, auto) {
        return ok(json::array({{{"guid", "marked"}, {"ts", 300}, {"duration", 1200}}})).dump();
    };
    for (const auto& resume : fntv::listResume(0, 12).Items)
        check(resume.Id != "marked", "marked item leaves continue watching");
    fntv::markLocalWatched(item, false);
    copy = item;
    fntv::applyLocalProgress(copy);
    check(!copy.UserData.Played, "unmark clears local completion");
}

void titlesAndImages() {
    jellyfin::Episode e;
    e.Name = "重逢";
    e.ParentIndexNumber = 1;
    e.IndexNumber = 2;
    check(fntv::episodeTitle(e) == "S1E2 · 重逢", "season and episode");
    check(fntv::episodeTitle(e, false) == "S1E2", "short label");
    e.ParentIndexNumber = 0;
    check(fntv::episodeTitle(e) == "第 2 集 · 重逢", "episode without season");
    e.Name = "第 2 集";
    check(fntv::episodeTitle(e) == "第 2 集", "no duplicated episode number");
    e.IndexNumber = 0;
    e.Name = "花絮";
    check(fntv::episodeTitle(e) == "花絮", "no S0E0 artefact");

    auto bare = fntv::mapItem({{"guid", "no-poster"}, {"title", "x"}, {"type", "Movie"}});
    check(bare.ImageTags.empty(), "item guid is not used as a poster path");
    auto poster = fntv::mapItem({{"guid", "p"}, {"poster", "/v/api/v1/sys/img/a.webp"}});
    check(poster.ImageTags.at(jellyfin::imageTypePrimary) == "/v/api/v1/sys/img/a.webp", "real poster kept");
    check(fntv::mapItem({{"guid", "c"}, {"type", "Collection"}}).Type == jellyfin::mediaTypeFolder,
        "collections open as folders");
}

void relativeFallbackUrl() {
    HTTP::respond = [](auto, const auto& url, auto) {
        if (url.find("/play/info") != std::string::npos)
            return ok({{"media_guid", "media"}, {"item", {{"guid", "movie"}, {"title", "M"}}}}).dump();
        if (url.find("/v/api/v1/stream") != std::string::npos && url.find("/stream/list") == std::string::npos)
            return ok({{"direct_link_qualities", json::array({{{"url", "/v/media/movie.m3u8"}}})}}).dump();
        return ok(json::object()).dump();
    };
    auto session = fntv::preparePlay("movie");
    check(session.fallback_url == "http://test.invalid/v/media/movie.m3u8", "relative fallback becomes absolute");
    check(session.direct_url == "http://test.invalid/v/api/v1/media/range/media", "direct range URL");
}

int main() {
    AppConfig::directory = "build-tests/compat-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(AppConfig::directory);
    const std::pair<const char*, void (*)()> cases[] = {
        {"business errors are not retried", businessErrorsAreNotRetried},
        {"network retry budget", networkRetryBudget},
        {"transport and business messages", transportMessages},
        {"invalid sign hint", invalidSignHint},
        {"login errors and server name", loginErrors},
        {"sort columns and fallback", sortAndFallback},
        {"watched state", watchedState},
        {"episode titles and posters", titlesAndImages},
        {"relative fallback url", relativeFallbackUrl},
    };
    for (const auto& test : cases) {
        try {
            test.second();
            std::cout << "PASS " << test.first << '\n';
        } catch (const std::exception& ex) {
            std::cerr << "FAIL " << test.first << ": " << ex.what() << '\n';
            return 1;
        }
    }
}
