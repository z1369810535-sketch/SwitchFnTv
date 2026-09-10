#pragma once

#include <functional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include <borealis.hpp>
#include "api/http.hpp"
#include "api/jellyfin.hpp"

namespace fntv {

using OnError = std::function<void(const std::string&)>;

constexpr const char* DEFAULT_BASE_URL = "http://192.168.31.137:5666";
constexpr const char* API_PREFIX = "/v/api/v1";
constexpr const char* APP_NAME = "trimemedia-web";
// Public client protocol constants from fntv-electron bb30d128. Not user passwords.
constexpr const char* API_KEY = "NDzZTVxnRKP8Z0jXg1VAMonaG8akvh";
constexpr const char* API_SECRET = "16CCEB3D-AB42-077D-36A1-F355324E4237";

std::string normalizeBaseUrl(std::string url);
std::string md5Hex(const std::string& text);
HTTP::Header signedHeaders(const std::string& path, const std::string& body, const std::string& token);
nlohmann::json requestJson(const std::string& method, const std::string& path, nlohmann::json body = nullptr,
    const std::string& baseOverride = "", const std::string& tokenOverride = "", long timeoutMs = 15000);
std::string requestRaw(const std::string& method, const std::string& path, nlohmann::json body = nullptr,
    const std::string& baseOverride = "", const std::string& tokenOverride = "", long timeoutMs = 20000);

struct LoginResult {
    std::string token;
    std::string user_id;
    std::string user_name;
    std::string server_id;
    std::string server_name;
};

struct SysConfigInfo {
    std::vector<std::string> field_names;
    bool has_nas_oauth = false;
};

struct PlaySession {
    std::string item_guid;
    std::string media_guid;
    std::string video_guid;
    std::string audio_guid;
    std::string subtitle_guid;
    std::string title;
    std::string direct_url;
    std::string fallback_url;
    std::string cookie_header;
    jellyfin::Source source;
};

LoginResult login(const std::string& base, const std::string& username, const std::string& password);
nlohmann::json sysConfig(const std::string& base);
SysConfigInfo inspectSysConfig(const nlohmann::json& cfg);
nlohmann::json userInfo();
std::vector<jellyfin::Collection> listLibraries();
jellyfin::Result<jellyfin::Episode> listItems(const std::string& parentGuid, size_t start, size_t pageSize,
    const std::string& itemType = "");
jellyfin::Detail getDetail(const std::string& guid);
jellyfin::Result<jellyfin::Episode> listSeasons(const std::string& seriesGuid);
jellyfin::Result<jellyfin::Episode> listEpisodes(const std::string& guid);
PlaySession preparePlay(const std::string& itemGuid);
void loadPoster(brls::Image* view, const jellyfin::Item& item);
void loadImagePath(brls::Image* view, const std::string& path);
std::string subtitleCacheDir();
void cleanupSubtitles();
std::string mapType(const std::string& raw);
std::string collectionType(const nlohmann::json& lib);

jellyfin::Episode mapItem(const nlohmann::json& j);
jellyfin::Detail mapDetail(const nlohmann::json& j);

template <typename Result>
inline void async(const std::function<Result()>& work, const std::function<void(Result)>& then, OnError error) {
    brls::async([work, then, error]() {
        try {
            Result r = work();
            brls::sync(std::bind(std::move(then), std::move(r)));
        } catch (const std::exception& ex) {
            if (error) brls::sync(std::bind(error, std::string(ex.what())));
        }
    });
}

}  // namespace fntv
