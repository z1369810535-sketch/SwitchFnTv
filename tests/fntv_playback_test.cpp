#include "api/fntv.hpp"
#include "utils/playback_history.hpp"
#include "utils/config.hpp"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>

using json = nlohmann::json;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
json ok(const json& data) { return {{"code", "0"}, {"data", data}}; }
jellyfin::Episode episode(const std::string& id) {
    auto item = fntv::mapItem({{"guid", id}, {"title", id}, {"type", "Episode"}, {"duration", 1800}, {"tv_guid", "show"}});
    return item;
}
void mockInfo(const json& position, bool include = true) {
    HTTP::respond = [position, include](const auto&, const auto& url, const auto& body) {
        if (url.find("/play/info") != std::string::npos) {
            auto request = json::parse(body);
            json info = {{"media_guid", "media"}, {"item", {{"guid", request.at("item_guid")},
                {"title", "Example"}, {"duration", "1800"}, {"watched_ts", "30"}}}};
            if (include) info["ts"] = position;
            return ok(info).dump();
        }
        return ok(json::object()).dump();
    };
}

void metadata() {
    const auto d = fntv::mapDetail({{"guid", "movie"}, {"title", "Title"}, {"rating", 8.7}, {"duration", "5400"},
        {"is_favorite", true}, {"is_watched", true}, {"ts", "120"}, {"backdrops", json::array({{{"path", "scene.jpg"}}})},
        {"actors", json::array({{{"name", "Actor"}, {"profile", "photo.jpg"}}})}});
    check(std::fabs(d.CommunityRating - 8.7) < 0.001, "decimal ratings survive mapping");
    check(d.RunTimeTicks == 5400 * jellyfin::PLAYTICKS, "detail duration preserved");
    check(d.UserData.IsFavorite && !d.UserData.Played && d.UserData.PlaybackPositionTicks == 120 * jellyfin::PLAYTICKS,
        "boolean flags and in-progress replays");
    check(d.BackdropImageTags.at(0) == "scene.jpg" && d.People.at(0).PrimaryImageTag == "photo.jpg", "0.9.7 image fields");
    const auto nulls = fntv::mapDetail({{"rating", nullptr}, {"duration", nullptr}, {"posters", nullptr}, {"genres", nullptr}});
    check(nulls.RunTimeTicks == 0 && nulls.CommunityRating == 0, "null metadata is safe");
    check(fntv::mapDetail({{"rating", "NaN"}}).CommunityRating == 0, "invalid rating is hidden");
}

void persistence() {
    auto file = AppConfig::directory + "/restart.json";
    auto item = episode("persist");
    item.MediaSources.push_back({});
    item.MediaSources.back().DirectStreamUrl = "https://private.invalid/?token=secret";
    {
        fntv::PlaybackHistory store(file);
        store.save(item, 123.5, 1800, false);
    }
    fntv::PlaybackHistory restarted(file);
    check(restarted.get(item.Id)->item.UserData.PlaybackPositionTicks == 1235000000, "resume after process restart");
    check(restarted.get(item.Id)->item.MediaSources.empty(), "stream credentials are never persisted");
    const auto first = restarted.save(item, 700, 1800, false);
    const auto rewind = restarted.save(item, 18, 1800, false);
    restarted.acknowledge(item.Id, first);
    check(restarted.get(item.Id)->pending, "old upload cannot acknowledge a newer rewind");
    restarted.acknowledge(item.Id, rewind);
    check(!restarted.get(item.Id)->pending && restarted.get(item.Id)->item.UserData.PlaybackPositionTicks == 18 * jellyfin::PLAYTICKS,
        "rewinding saves the smaller position");
    restarted.save(item, 1800, 1800, true);
    check(restarted.get(item.Id)->item.UserData.Played && restarted.get(item.Id)->item.UserData.PlaybackPositionTicks == 0, "EOF marks completion");
    auto count = restarted.records().size();
    check(restarted.save(item, std::numeric_limits<double>::quiet_NaN(), 1800, false) == 0, "reject non-finite timestamps");
    check(restarted.save(item, -1, 1800, false) == 0 && restarted.records().size() == count, "reject negative timestamps");
    restarted.save(item, 99999, 1800, false);
    check(restarted.get(item.Id)->item.UserData.PlaybackPositionTicks == 1800 * jellyfin::PLAYTICKS, "bound progress to duration");
    restarted.save(item, 24, 1800, false);
    { std::ofstream corrupt(file); corrupt << "{"; }
    fntv::PlaybackHistory recovered(file);
    check(recovered.get(item.Id).has_value(), "recover interrupted primary from backup");
    fntv::PlaybackHistory other(AppConfig::directory + "/another-account.json");
    check(!other.get(item.Id).has_value(), "histories do not cross accounts");
}

void resumeVariants() {
    for (const auto& ts : {json(125), json("125")}) {
        mockInfo(ts);
        auto session = fntv::preparePlay("remote");
        check(session.resume_ticks == 125 * jellyfin::PLAYTICKS, "play/info ts overrides item watched_ts");
        check(session.base_url == AppConfig::url && session.token == "test-token", "capture account for playback");
    }
    mockInfo(0);
    check(fntv::preparePlay("remote").resume_ticks == 0, "explicit server zero overrides stale item position");
    mockInfo(nullptr);
    check(fntv::preparePlay("remote").resume_ticks == 30 * jellyfin::PLAYTICKS, "null timestamp falls back to item");
    mockInfo(-50);
    check(fntv::preparePlay("remote").resume_ticks >= 0, "negative remote timestamp cannot become huge unsigned seek");
    mockInfo(99999);
    check(fntv::preparePlay("remote").resume_ticks == 1800 * jellyfin::PLAYTICKS, "resume clamped to media duration");
}

void offlineAndOrderedSync() {
    mockInfo(0);
    auto session = fntv::preparePlay("offline");
    HTTP::respond = [](auto, auto, auto) -> std::string { throw std::runtime_error("offline"); };
    fntv::savePlaybackProgress(session, 420, 1800);
    fntv::PlaybackHistory disk(session.history_file);
    check(disk.get("offline")->pending, "failed sync stays pending on disk");
    mockInfo(0);
    check(fntv::preparePlay("offline").resume_ticks == 420 * jellyfin::PLAYTICKS, "pending local progress survives stale server zero");
    int uploads = 0;
    HTTP::respond = [&uploads](const auto& method, const auto& url, const auto& body) {
        check(method == "POST" && url == "http://test.invalid/v/api/v1/play/record", "native playback record endpoint and original server");
        const auto request = json::parse(body);
        check(request.at("ts") == 12 && request.at("duration") == 1800, "upload latest rewind in seconds");
        check(request.at("item_guid") == "offline" && request.at("media_guid") == "media", "upload correct playback session");
        ++uploads;
        return ok(nullptr).dump();
    };
    brls::deferAsync = true;
    fntv::savePlaybackProgress(session, 500, 1800);
    fntv::savePlaybackProgress(session, 12, 1800);
    AppConfig::url = "http://other.invalid";
    AppConfig::user = "other-user";
    for (auto it = brls::jobs.rbegin(); it != brls::jobs.rend(); ++it) (*it)();
    brls::jobs.clear();
    brls::deferAsync = false;
    check(uploads == 1, "out-of-order queued reports cannot overwrite latest progress");
    auto other = episode("offline");
    fntv::applyLocalProgress(other);
    check(other.UserData.PlaybackPositionTicks == 0, "server/user switch does not reuse former progress");
    AppConfig::url = "http://test.invalid";
    AppConfig::user = "test-user";
}

void episodeSelectionAndHome() {
    mockInfo(0);
    auto session = fntv::preparePlay("e2");
    auto first = episode("e1"), second = episode("e2"), third = episode("e3");
    std::vector<jellyfin::Episode> all{first, second, third};
    fntv::savePlaybackProgress(session, 600, 1800);
    check(fntv::selectResumeEpisode(all) == 1, "series resumes last played episode, not first");
    fntv::savePlaybackProgress(session, 1800, 1800, true);
    check(fntv::selectResumeEpisode(all) == 2, "completed episode advances to the next episode");
    HTTP::respond = [](auto, const auto& url, auto) {
        check(url.find("/play/list") != std::string::npos, "continue watching endpoint");
        return ok(json::array({{{"guid", "e2"}, {"ts", 500}}, {{"item", {{"guid", "server"}, {"duration", "1800"}}}, {"ts", "200"}}})).dump();
    };
    const auto home = fntv::listResume(0, 20);
    check(home.Items.at(0).Id == "server" && home.Items.at(0).UserData.PlaybackPositionTicks == 200 * jellyfin::PLAYTICKS,
        "nested continue-watching item and root ts");
    for (const auto& item : home.Items) check(item.Id != "e2", "completed episode suppressed even in stale server list");
    check(fntv::listResume(999, 12).Items.empty(), "continue-watching pagination terminates");
    HTTP::respond = [](auto, auto, auto) -> std::string { throw std::runtime_error("offline"); };
    check(!fntv::listResume(0, 12).Items.empty(), "offline home keeps local resume entries");
}

void favorites() {
    int calls = 0;
    HTTP::respond = [&calls](const auto& method, const auto& url, const auto& body) {
        check(method == (calls++ == 0 ? "PUT" : "DELETE"), "favorite uses actual PUT and DELETE methods");
        check(url.find("/item/favorite") != std::string::npos && json::parse(body).at("item_guid") == "movie", "native favorite payload");
        return ok(nullptr).dump();
    };
    check(fntv::setFavorite("movie", true) && !fntv::setFavorite("movie", false), "favorite result state");
}

int main() {
    AppConfig::directory = "build-tests/playback-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(AppConfig::directory);
    const std::pair<const char*, void(*)()> cases[] = {{"detail compatibility", metadata}, {"restart, rewind and recovery", persistence},
        {"server resume formats", resumeVariants}, {"offline, upload ordering and account isolation", offlineAndOrderedSync},
        {"episode resume, completion and home", episodeSelectionAndHome}, {"native favorites", favorites}};
    for (const auto& test : cases) {
        try { test.second(); std::cout << "PASS " << test.first << '\n'; }
        catch (const std::exception& ex) { std::cerr << "FAIL " << test.first << ": " << ex.what() << '\n'; return 1; }
    }
}
