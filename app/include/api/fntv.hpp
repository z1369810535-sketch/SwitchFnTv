#pragma once

#include <functional>
#include <stdexcept>
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

/// Why a request failed. The UI uses this to decide between "retry", "log in again"
/// and "show the server's message" instead of parsing localized text.
enum class ErrorKind {
    Network,  // NAS unreachable, timeout, connection reset, HTTP 5xx
    Auth,     // token rejected / expired, wrong credentials
    Server,   // the API answered with a business error (code != 0) or HTTP 4xx
    Format,   // the response could not be understood
};

class ApiError : public std::runtime_error {
public:
    ApiError(ErrorKind kind, const std::string& message, int code = 0, std::string raw = "", bool retryable = false)
        : std::runtime_error(message), kind(kind), code(code), raw(std::move(raw)), retryable(retryable) {}
    ErrorKind kind;
    int code;          // business code or HTTP status, 0 when unknown
    std::string raw;   // untranslated server / transport message
    bool retryable;    // safe to retry the same request
};

std::string normalizeBaseUrl(std::string url);
std::string md5Hex(const std::string& text);
HTTP::Header signedHeaders(const std::string& path, const std::string& body, const std::string& token);
/// attempts: total tries for transient network failures. Business errors are never retried
/// (retrying a rejected login can trigger the NAS lockout), "invalid sign" is retried separately.
nlohmann::json requestJson(const std::string& method, const std::string& path, nlohmann::json body = nullptr,
    const std::string& baseOverride = "", const std::string& tokenOverride = "", long timeoutMs = 15000,
    int attempts = 2);
std::string requestRaw(const std::string& method, const std::string& path, nlohmann::json body = nullptr,
    const std::string& baseOverride = "", const std::string& tokenOverride = "", long timeoutMs = 20000,
    int attempts = 2);
/// Translate a transport failure (curl message / "http status N") into a readable ApiError.
ApiError transportError(const std::string& raw, const std::string& base);
/// Translate a business error (code != 0) into a readable ApiError.
ApiError businessError(int code, const std::string& msg);

/// Sort columns accepted by item/list. label is an i18n key.
struct SortOption {
    std::string column;
    std::string label;
};
const std::vector<SortOption>& sortOptions();
constexpr const char* SORT_NAME = "sort_title";
constexpr const char* SORT_ADDED = "create_time";

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
    std::string base_url;
    std::string token;
    std::string history_file;
    int64_t resume_ticks = 0;
    jellyfin::Episode item;
    std::string item_guid;
    std::string media_guid;
    std::string video_guid;
    std::string audio_guid;
    std::string subtitle_guid;
    std::string title;
    std::string direct_url;
    std::string fallback_url;
    std::string cookie_header;
    jellyfin::Source source{};
};

LoginResult login(const std::string& base, const std::string& username, const std::string& password);
nlohmann::json sysConfig(const std::string& base, long timeoutMs = 15000, int attempts = 2);
SysConfigInfo inspectSysConfig(const nlohmann::json& cfg);
/// Display name of the NAS from sys/config (server_name on 0.9.x).
std::string serverName(const nlohmann::json& cfg, const std::string& fallback = "飞牛影视");
/// Validates a session. Empty base/token use the active account.
nlohmann::json userInfo(const std::string& base = "", const std::string& token = "", long timeoutMs = 15000,
    int attempts = 2);
/// Best effort: tells the NAS to drop the session. Never throws.
void logout(const std::string& base, const std::string& token);
std::vector<jellyfin::Collection> listLibraries();
/// sortColumn/sortType empty = name ascending. An unsupported sort column falls back to name order.
jellyfin::Result<jellyfin::Episode> listItems(const std::string& parentGuid, size_t start, size_t pageSize,
    const std::string& itemType = "", const std::string& sortColumn = "", const std::string& sortType = "");
jellyfin::Detail getDetail(const std::string& guid);
jellyfin::Result<jellyfin::Episode> listSeasons(const std::string& seriesGuid);
jellyfin::Result<jellyfin::Episode> listEpisodes(const std::string& guid);
jellyfin::Result<jellyfin::Episode> listSeriesEpisodes(const std::string& seriesGuid);
PlaySession preparePlay(const std::string& itemGuid);
void savePlaybackProgress(const PlaySession& session, double seconds, double duration, bool completed = false);
void applyLocalProgress(jellyfin::Item& item);
size_t selectResumeEpisode(const std::vector<jellyfin::Episode>& items);
jellyfin::Result<jellyfin::Episode> listResume(size_t start, size_t pageSize);
bool setFavorite(const std::string& guid, bool favorite);
/// Marks an item watched/unwatched on the NAS (item/watched PUT/DELETE).
bool setWatched(const std::string& guid, bool watched);
/// Mirrors a watched/unwatched mark into the local playback history so home, resume
/// and series "play" agree with the mark even when the NAS could not be updated.
void markLocalWatched(const jellyfin::Episode& item, bool watched);
/// "S1E2 · 名称" / "第 2 集 · 名称" / "名称" without "S0E0" artefacts.
std::string episodeTitle(const jellyfin::Episode& item, bool withName = true);
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
