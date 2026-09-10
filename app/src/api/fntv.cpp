#include "api/fntv.hpp"
#include "utils/config.hpp"
#include "utils/image.hpp"
#include "utils/misc.hpp"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace fntv {
namespace {

constexpr uint32_t MD5_K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};
constexpr uint32_t MD5_S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14,
    20, 5, 9, 14, 20, 5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10,
    15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

uint32_t rotl(uint32_t x, uint32_t n) { return (x << n) | (x >> (32 - n)); }

std::string toHex(const uint8_t* data, size_t len) {
    static const char* kHex = "0123456789abcdef";
    std::string out(len * 2, '0');
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = kHex[data[i] >> 4];
        out[i * 2 + 1] = kHex[data[i] & 0xf];
    }
    return out;
}

std::string dumpBody(const nlohmann::json& body) {
    if (body.is_null()) return "";
    return body.dump(-1, ' ', false);
}

std::mutex rngMutex;
std::mt19937 rng{static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count())};

std::string randomNonce() {
    std::uniform_int_distribution<int> dist(100000, 999999);
    std::lock_guard<std::mutex> lock(rngMutex);
    return std::to_string(dist(rng));
}

std::string timestampMs() {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    return std::to_string(ms);
}

std::string currentBase(const std::string& overrideUrl) {
    if (!overrideUrl.empty()) return normalizeBaseUrl(overrideUrl);
    try {
        const auto& url = AppConfig::instance().getUrl();
        if (!url.empty()) return normalizeBaseUrl(url);
    } catch (...) {
    }
    return DEFAULT_BASE_URL;
}

std::string currentToken(const std::string& overrideToken) {
    if (!overrideToken.empty()) return overrideToken;
    try {
        return AppConfig::instance().getToken();
    } catch (...) {
        return "";
    }
}

std::string genAuthx(const std::string& path, const std::string& body) {
    const std::string nonce = randomNonce();
    const std::string ts = timestampMs();
    const std::string bodyMd5 = md5Hex(body);
    const std::string signSrc = std::string(API_KEY) + "_" + path + "_" + nonce + "_" + ts + "_" + bodyMd5 + "_" + API_SECRET;
    return "nonce=" + nonce + "&timestamp=" + ts + "&sign=" + md5Hex(signSrc);
}

std::string humanError(const std::string& msg, int code) {
    if (msg.find("password") != std::string::npos || msg.find("user") != std::string::npos ||
        msg.find("account") != std::string::npos || code == 401)
        return "登录失败，请检查用户名和密码";
    if (msg.find("timeout") != std::string::npos || msg.find("Timeout") != std::string::npos)
        return "连接超时，请检查 NAS 地址和局域网";
    if (msg.find("Could not resolve") != std::string::npos || msg.find("Couldn't connect") != std::string::npos ||
        msg.find("Failed to connect") != std::string::npos)
        return "无法连接服务器，请检查地址 " + currentBase("");
    if (!msg.empty()) return msg;
    return "飞牛接口错误 (" + std::to_string(code) + ")";
}

nlohmann::json asArray(const nlohmann::json& data, const char* key = "list") {
    if (data.is_array()) return data;
    if (data.is_object() && data.contains(key) && data[key].is_array()) return data[key];
    return nlohmann::json::array();
}

long jsonLong(const nlohmann::json& j, const char* key, long fallback = 0) {
    if (!j.is_object() || !j.contains(key) || j[key].is_null()) return fallback;
    if (j[key].is_number()) return static_cast<long>(j[key].get<double>());
    if (j[key].is_string()) {
        try {
            return std::stol(j[key].get<std::string>());
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

std::string jsonString(const nlohmann::json& j, std::initializer_list<const char*> keys, const std::string& fallback = "") {
    if (!j.is_object()) return fallback;
    for (auto key : keys) {
        if (j.contains(key) && j[key].is_string()) {
            auto s = j[key].get<std::string>();
            if (!s.empty()) return s;
        }
        if (j.contains(key) && j[key].is_number()) return std::to_string(static_cast<long long>(j[key].get<double>()));
    }
    return fallback;
}

int jsonInt(const nlohmann::json& j, const char* key, int fallback = 0) {
    if (!j.is_object() || !j.contains(key) || j[key].is_null()) return fallback;
    if (j[key].is_number_integer()) return j[key].get<int>();
    if (j[key].is_number()) return static_cast<int>(j[key].get<double>());
    if (j[key].is_string()) {
        try {
            return std::stoi(j[key].get<std::string>());
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

std::string firstImagePath(const nlohmann::json& j) {
    static const char* keys[] = {
        "poster", "posters", "still_path", "poster_path", "backdrop", "profile_path", "avatar", "cover"};
    if (!j.is_object()) return "";
    for (auto key : keys) {
        if (!j.contains(key)) continue;
        const auto& v = j[key];
        if (v.is_string()) {
            auto s = v.get<std::string>();
            if (!s.empty() && s != "null") return s;
        } else if (v.is_array() && !v.empty()) {
            if (v[0].is_string()) {
                auto s = v[0].get<std::string>();
                if (!s.empty()) return s;
            } else if (v[0].is_object()) {
                auto s = jsonString(v[0], {"url", "path", "poster", "src", "profile_path"});
                if (!s.empty()) return s;
            }
        } else if (v.is_object()) {
            auto s = jsonString(v, {"url", "path", "poster", "src", "profile_path"});
            if (!s.empty()) return s;
        }
    }
    return "";
}

std::string posterOf(const nlohmann::json& j) {
    auto p = firstImagePath(j);
    if (!p.empty()) return p;
    return jsonString(j, {"guid", "id"});
}

std::string resolveImageRel(std::string path) {
    while (!path.empty() && (path.front() == ' ' || path.front() == '\t')) path.erase(path.begin());
    while (!path.empty() && (path.back() == ' ' || path.back() == '\t')) path.pop_back();
    if (path.empty()) return "";
    if (path.rfind("http://", 0) == 0 || path.rfind("https://", 0) == 0) return path;
    if (path.rfind("//", 0) == 0) return std::string("http:") + path;
    std::string query;
    auto q = path.find('?');
    if (q != std::string::npos) {
        query = path.substr(q);
        path = path.substr(0, q);
    }
    if (path.rfind("/v/", 0) == 0) return path + query;
    if (path.rfind("v/", 0) == 0) return "/" + path + query;
    if (path.rfind("/api/", 0) == 0) return "/v" + path + query;
    if (path.rfind("/sys/img/", 0) == 0) return "/v/api/v1" + path + query;
    if (!path.empty() && path.front() == '/') path.erase(path.begin());
    return std::string("/v/api/v1/sys/img/") + path + query;
}

HTTP::Header imageHeaders(const std::string& urlOrRel, const std::string& token) {
    std::string path = urlOrRel;
    auto scheme = path.find("://");
    if (scheme != std::string::npos) {
        auto slash = path.find('/', scheme + 3);
        path = slash == std::string::npos ? "/" : path.substr(slash);
    }
    auto q = path.find('?');
    if (q != std::string::npos) path = path.substr(0, q);
    return {
        "Authorization: " + token,
        "Authx: " + genAuthx(path, ""),
        "Accept: image/webp,image/jpeg,image/png,image/*,*/*",
    };
}

jellyfin::MediaPeople mapPerson(const nlohmann::json& j, const std::string& defaultRole = "") {
    jellyfin::MediaPeople p;
    p.Id = jsonString(j, {"guid", "id", "person_guid"});
    p.Name = jsonString(j, {"name", "title", "person_name", "actor_name"});
    p.Role = jsonString(j, {"role", "character", "job", "department", "type"}, defaultRole);
    p.PrimaryImageTag = firstImagePath(j);
    if (p.PrimaryImageTag.empty()) p.PrimaryImageTag = jsonString(j, {"profile_path", "poster", "avatar"});
    return p;
}

void appendPeople(std::vector<jellyfin::MediaPeople>& out, const nlohmann::json& j, const char* key, const std::string& role) {
    if (!j.is_object() || !j.contains(key)) return;
    const auto& v = j[key];
    auto push = [&](jellyfin::MediaPeople p) {
        if (p.Name.empty()) return;
        if (p.Id.empty()) p.Id = p.Name;
        if (p.Role.empty()) p.Role = role;
        out.push_back(std::move(p));
    };
    if (v.is_array()) {
        for (auto& it : v) {
            if (it.is_string()) {
                jellyfin::MediaPeople p;
                p.Name = it.get<std::string>();
                p.Role = role;
                p.Id = p.Name;
                push(std::move(p));
            } else if (it.is_object()) {
                push(mapPerson(it, role));
            }
        }
    } else if (v.is_string()) {
        jellyfin::MediaPeople p;
        p.Name = v.get<std::string>();
        p.Role = role;
        p.Id = p.Name;
        push(std::move(p));
    } else if (v.is_object()) {
        push(mapPerson(v, role));
    }
}

std::vector<jellyfin::MediaPeople> mapPeople(const nlohmann::json& j) {
    std::vector<jellyfin::MediaPeople> out;
    appendPeople(out, j, "people", "");
    appendPeople(out, j, "persons", "");
    appendPeople(out, j, "actors", "演员");
    appendPeople(out, j, "actor", "演员");
    appendPeople(out, j, "casts", "演员");
    appendPeople(out, j, "cast", "演员");
    appendPeople(out, j, "directors", "导演");
    appendPeople(out, j, "director", "导演");
    appendPeople(out, j, "writers", "编剧");
    appendPeople(out, j, "writer", "编剧");
    return out;
}

std::vector<jellyfin::MediaPeople> fetchPeople(const std::string& itemGuid) {
    nlohmann::json data;
    try {
        data = requestJson("POST", std::string("/v/api/v1/person/list/") + itemGuid, nlohmann::json::object());
    } catch (...) {
        try {
            data = requestJson("GET", std::string("/v/api/v1/person/list/") + itemGuid);
        } catch (...) {
            data = requestJson("POST", "/v/api/v1/person/list", nlohmann::json{{"item_guid", itemGuid}});
        }
    }
    auto arr = asArray(data);
    if (arr.empty()) arr = asArray(data, "people");
    if (arr.empty()) arr = asArray(data, "items");
    if (arr.empty()) arr = asArray(data, "actors");
    std::vector<jellyfin::MediaPeople> out;
    for (auto& it : arr) {
        auto p = mapPerson(it);
        if (p.Name.empty()) continue;
        if (p.Id.empty()) p.Id = p.Name;
        out.push_back(std::move(p));
    }
    return out;
}

void fillUserData(jellyfin::UserDataResult& u, const nlohmann::json& j, uint64_t runtimeTicks) {
    u.IsFavorite = jsonInt(j, "is_favorite", 0) != 0;
    u.Played = jsonInt(j, "is_watched", jsonInt(j, "watched", 0)) != 0;
    const int64_t ts = jsonLong(j, "watched_ts", 0);
    if (ts > 0) u.PlaybackPositionTicks = static_cast<uint64_t>(ts) * jellyfin::PLAYTICKS;
    if (runtimeTicks > 0 && ts > 0) {
        u.PlayedPercentage = 100.0f * static_cast<float>(ts) / static_cast<float>(runtimeTicks / jellyfin::PLAYTICKS);
    }
}

std::string requestOnce(const std::string& method, const std::string& url, const std::string& body, const HTTP::Header& header,
    long timeoutMs) {
    HTTP::Timeout timeout{timeoutMs};
    if (method == "GET" || method == "get") return HTTP::get(url, header, timeout);
    return HTTP::post(url, body, header, timeout);
}

}  // namespace

std::string md5Hex(const std::string& text) {
    uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
    std::vector<uint8_t> msg(text.begin(), text.end());
    const uint64_t bitLen = static_cast<uint64_t>(msg.size()) * 8;
    msg.push_back(0x80);
    while ((msg.size() % 64) != 56) msg.push_back(0);
    for (int i = 0; i < 8; i++) msg.push_back(static_cast<uint8_t>((bitLen >> (8 * i)) & 0xff));

    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[16];
        for (int i = 0; i < 16; i++) {
            w[i] = static_cast<uint32_t>(msg[off + 4 * i]) | (static_cast<uint32_t>(msg[off + 4 * i + 1]) << 8) |
                   (static_cast<uint32_t>(msg[off + 4 * i + 2]) << 16) | (static_cast<uint32_t>(msg[off + 4 * i + 3]) << 24);
        }
        uint32_t a = a0, b = b0, c = c0, d = d0;
        for (uint32_t i = 0; i < 64; i++) {
            uint32_t f, g;
            if (i < 16) {
                f = (b & c) | ((~b) & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | ((~d) & c);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3 * i + 5) % 16;
            } else {
                f = c ^ (b | (~d));
                g = (7 * i) % 16;
            }
            const uint32_t tmp = d;
            d = c;
            c = b;
            b = b + rotl(a + f + MD5_K[i] + w[g], MD5_S[i]);
            a = tmp;
        }
        a0 += a;
        b0 += b;
        c0 += c;
        d0 += d;
    }
    uint8_t digest[16];
    auto store = [&](uint32_t v, int i) {
        digest[i] = static_cast<uint8_t>(v);
        digest[i + 1] = static_cast<uint8_t>(v >> 8);
        digest[i + 2] = static_cast<uint8_t>(v >> 16);
        digest[i + 3] = static_cast<uint8_t>(v >> 24);
    };
    store(a0, 0);
    store(b0, 4);
    store(c0, 8);
    store(d0, 12);
    return toHex(digest, 16);
}

std::string normalizeBaseUrl(std::string url) {
    auto trim = [](std::string& s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '/')) s.pop_back();
    };
    trim(url);
    if (url.empty()) return DEFAULT_BASE_URL;
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) url = "http://" + url;
    auto loginPos = url.find("/v/login");
    if (loginPos != std::string::npos) url = url.substr(0, loginPos);
    if (url.size() >= 2 && url.compare(url.size() - 2, 2, "/v") == 0) url.resize(url.size() - 2);
    trim(url);
    return url;
}

HTTP::Header signedHeaders(const std::string& path, const std::string& body, const std::string& token) {
    return {
        "Content-Type: application/json",
        "Authorization: " + token,
        "Authx: " + genAuthx(path, body),
    };
}

std::string requestRaw(const std::string& method, const std::string& path, nlohmann::json body, const std::string& baseOverride,
    const std::string& tokenOverride, long timeoutMs) {
    const std::string base = currentBase(baseOverride);
    const std::string token = currentToken(tokenOverride);
    std::string lastError = "飞牛接口请求失败";
    for (int attempt = 0; attempt < 5; attempt++) {
        nlohmann::json payload = body;
        if (method == "POST" || method == "PUT" || method == "post" || method == "put") {
            if (payload.is_null() || payload.empty()) payload = nlohmann::json::object();
            payload["nonce"] = randomNonce();
        }
        const std::string bodyStr = payload.is_null() ? std::string() : dumpBody(payload);
        const auto header = signedHeaders(path, bodyStr, token);
        try {
            const std::string resp = requestOnce(method, base + path, bodyStr, header, timeoutMs);
            auto parsed = nlohmann::json::parse(resp, nullptr, false);
            if (parsed.is_discarded()) return resp;
            const int code = parsed.value("code", 0);
            const std::string msg = parsed.value("msg", std::string());
            if (code == 5000 && msg == "invalid sign") {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            if (parsed.contains("code") && code != 0) throw std::runtime_error(humanError(msg, code));
            if (parsed.contains("data") && parsed["data"].is_string()) return parsed["data"].get<std::string>();
            return resp;
        } catch (const std::exception& ex) {
            lastError = humanError(ex.what(), 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    throw std::runtime_error(lastError);
}

nlohmann::json requestJson(const std::string& method, const std::string& path, nlohmann::json body, const std::string& baseOverride,
    const std::string& tokenOverride, long timeoutMs) {
    const std::string base = currentBase(baseOverride);
    const std::string token = currentToken(tokenOverride);
    std::string lastError = "飞牛接口请求失败";
    for (int attempt = 0; attempt < 5; attempt++) {
        nlohmann::json payload = body;
        if (method == "POST" || method == "PUT" || method == "post" || method == "put") {
            if (payload.is_null() || !payload.is_object()) payload = nlohmann::json::object();
            payload["nonce"] = randomNonce();
        }
        const std::string bodyStr = payload.is_null() ? std::string() : dumpBody(payload);
        const auto header = signedHeaders(path, bodyStr, token);
        try {
            const std::string resp = requestOnce(method, base + path, bodyStr, header, timeoutMs);
            auto parsed = nlohmann::json::parse(resp, nullptr, false);
            if (parsed.is_discarded() || !parsed.is_object()) throw std::runtime_error("服务器返回了无法解析的数据");
            const int code = parsed.value("code", 0);
            std::string msg;
            if (parsed.contains("msg") && parsed["msg"].is_string()) msg = parsed["msg"].get<std::string>();
            if (code == 5000 && msg == "invalid sign") {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            if (parsed.contains("code") && code != 0) throw std::runtime_error(humanError(msg, code));
            if (parsed.contains("data")) return parsed["data"];
            return parsed;
        } catch (const std::exception& ex) {
            lastError = humanError(ex.what(), 0);
            if (std::string(ex.what()).find("invalid sign") == std::string::npos && attempt >= 2) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    throw std::runtime_error(lastError);
}

std::string mapType(const std::string& raw) {
    std::string t = raw;
    for (auto& c : t) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    if (t == "movie" || t == "movies" || t == "film") return jellyfin::mediaTypeMovie;
    if (t == "tv" || t == "tvshow" || t == "series" || t == "show" || t == "tvshows") return jellyfin::mediaTypeSeries;
    if (t == "season") return jellyfin::mediaTypeSeason;
    if (t == "episode") return jellyfin::mediaTypeEpisode;
    if (t == "folder" || t == "directory" || t == "dir") return jellyfin::mediaTypeFolder;
    if (t == "video") return jellyfin::mediaTypeMovie;
    return raw.empty() ? jellyfin::mediaTypeMovie : raw;
}

std::string collectionType(const nlohmann::json& lib) {
    auto raw = jsonString(lib, {"category", "type", "mdb_category", "collection_type"});
    std::string t = raw;
    for (auto& c : t) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    if (t.find("movie") != std::string::npos || t == "film") return "movies";
    if (t.find("tv") != std::string::npos || t.find("show") != std::string::npos || t.find("series") != std::string::npos)
        return "tvshows";
    return "";
}

jellyfin::Episode mapItem(const nlohmann::json& j) {
    jellyfin::Episode item;
    item.Id = jsonString(j, {"guid", "id", "item_guid"});
    item.Name = jsonString(j, {"title", "name", "tv_title"});
    item.Type = mapType(jsonString(j, {"type"}, "Movie"));
    const auto poster = posterOf(j);
    if (!poster.empty()) item.ImageTags[jellyfin::imageTypePrimary] = poster;
    const auto date = jsonString(j, {"air_date", "release_date", "premiere_date"});
    if (date.size() >= 4) item.ProductionYear = std::strtol(date.c_str(), nullptr, 10);
    else item.ProductionYear = jsonLong(j, "year", 0);
    const double duration = static_cast<double>(jsonLong(j, "duration", 0));
    const double runtime = static_cast<double>(jsonLong(j, "runtime", 0));
    if (duration > 0) item.RunTimeTicks = static_cast<uint64_t>(duration) * jellyfin::PLAYTICKS;
    else if (runtime > 0) item.RunTimeTicks = static_cast<uint64_t>(runtime * 60.0) * jellyfin::PLAYTICKS;
    fillUserData(item.UserData, j, item.RunTimeTicks);
    item.Overview = jsonString(j, {"overview", "desc", "description"});
    item.IndexNumber = jsonInt(j, "episode_number", jsonInt(j, "index", 0));
    item.ParentIndexNumber = jsonInt(j, "season_number", 0);
    item.SeriesName = jsonString(j, {"tv_title", "series_name", "parent_title"});
    const auto parent = jsonString(j, {"ancestor_guid", "series_guid", "parent_guid"});
    if (!parent.empty()) item.SeriesId = parent;
    if (item.Type == jellyfin::mediaTypeSeason) item.SeriesId = jsonString(j, {"parent_guid", "ancestor_guid"});
    return item;
}

jellyfin::Detail mapDetail(const nlohmann::json& j) {
    jellyfin::Detail d;
    auto ep = mapItem(j);
    d.Id = ep.Id;
    d.Name = ep.Name;
    d.Type = ep.Type;
    d.ImageTags = ep.ImageTags;
    d.ProductionYear = ep.ProductionYear;
    d.Overview = ep.Overview;
    d.UserData = ep.UserData;
    d.CommunityRating = 0.f;
    auto rating = jsonString(j, {"vote_average", "rating"});
    if (!rating.empty()) {
        try {
            d.CommunityRating = std::stof(rating);
        } catch (...) {
        }
    } else if (j.contains("vote_average") && j["vote_average"].is_number()) {
        d.CommunityRating = j["vote_average"].get<float>();
    }
    if (j.contains("genres") && j["genres"].is_array()) {
        for (auto& g : j["genres"]) {
            if (g.is_string()) d.Genres.push_back(g.get<std::string>());
            else if (g.is_object()) d.Genres.push_back(jsonString(g, {"name", "title"}));
        }
    } else {
        auto genre = jsonString(j, {"genre", "genres"});
        if (!genre.empty()) d.Genres.push_back(genre);
    }
    d.People = mapPeople(j);
    return d;
}

LoginResult login(const std::string& base, const std::string& username, const std::string& password) {
    nlohmann::json body = {
        {"app_name", APP_NAME},
        {"username", username},
        {"password", password},
    };
    auto data = requestJson("POST", "/v/api/v1/login", body, base, "");
    LoginResult r;
    if (data.is_string()) {
        r.token = data.get<std::string>();
    } else if (data.is_object()) {
        r.token = jsonString(data, {"token", "access_token", "Authorization", "authorization"});
        r.user_id = jsonString(data, {"guid", "id", "user_id", "uid"}, username);
        r.user_name = jsonString(data, {"username", "nickname", "name"}, username);
    }
    if (r.token.empty()) throw std::runtime_error("登录成功但未返回会话令牌");
    if (r.user_id.empty()) r.user_id = username;
    if (r.user_name.empty()) r.user_name = username;
    r.server_id = md5Hex(normalizeBaseUrl(base));
    r.server_name = "飞牛影视";
    try {
        auto cfg = sysConfig(base);
        auto name = jsonString(cfg, {"name", "server_name", "title"});
        if (!name.empty()) r.server_name = name;
    } catch (...) {
    }
    return r;
}

nlohmann::json sysConfig(const std::string& base) { return requestJson("GET", "/v/api/v1/sys/config", nullptr, base, ""); }

SysConfigInfo inspectSysConfig(const nlohmann::json& cfg) {
    SysConfigInfo info;
    if (cfg.is_object()) {
        for (auto it = cfg.begin(); it != cfg.end(); ++it) info.field_names.push_back(it.key());
        info.has_nas_oauth = cfg.contains("nas_oauth") || cfg.contains("oauth") || cfg.contains("nasOAuth");
    }
    return info;
}

nlohmann::json userInfo() { return requestJson("GET", "/v/api/v1/user/info"); }

std::vector<jellyfin::Collection> listLibraries() {
    nlohmann::json data;
    try {
        data = requestJson("GET", "/v/api/v1/mediadb/list");
    } catch (...) {
        data = requestJson("GET", "/v/api/v1/mdb/list");
    }
    std::vector<jellyfin::Collection> out;
    for (auto& it : asArray(data)) {
        jellyfin::Collection c;
        c.Id = jsonString(it, {"guid", "id", "mdb_guid"});
        c.Name = jsonString(it, {"name", "mdb_name", "title"});
        c.Type = jellyfin::mediaTypeFolder;
        c.IsFolder = true;
        c.CollectionType = collectionType(it);
        auto poster = posterOf(it);
        if (!poster.empty()) c.ImageTags[jellyfin::imageTypePrimary] = poster;
        if (!c.Id.empty()) out.push_back(std::move(c));
    }
    return out;
}

jellyfin::Result<jellyfin::Episode> listItems(const std::string& parentGuid, size_t start, size_t pageSize, const std::string& itemType) {
    const int page = static_cast<int>(pageSize ? start / pageSize + 1 : 1);
    const int size = static_cast<int>(pageSize ? pageSize : 50);
    const bool drillDown = itemType == jellyfin::mediaTypeSeason || itemType == jellyfin::mediaTypeEpisode;

    nlohmann::json req = {
        {"sort_column", "sort_title"},
        {"sort_type", "ASC"},
        {"page", page},
        {"page_size", size},
    };
    if (drillDown) {
        req["parent_guid"] = parentGuid;
        req["exclude_folder"] = 0;
    } else {
        req["ancestor_guid"] = parentGuid;
        req["exclude_grouped_video"] = 1;
        nlohmann::json types = nlohmann::json::array({"Movie", "TV", "Directory", "Video"});
        if (itemType == jellyfin::mediaTypeSeries) types = nlohmann::json::array({"TV"});
        else if (itemType == jellyfin::mediaTypeMovie) types = nlohmann::json::array({"Movie"});
        req["tags"] = nlohmann::json{{"type", types}};
    }

    auto data = requestJson("POST", "/v/api/v1/item/list", req);
    auto arr = asArray(data);
    if (arr.empty() && !drillDown) {
        data = requestJson("POST", "/v/api/v1/item/list", nlohmann::json{
            {"parent_guid", parentGuid},
            {"exclude_folder", 0},
            {"sort_column", "sort_title"},
            {"sort_type", "ASC"},
            {"page", page},
            {"page_size", size},
        });
        arr = asArray(data);
    }

    jellyfin::Result<jellyfin::Episode> r;
    r.StartIndex = static_cast<long>(start);
    r.TotalRecordCount = data.is_object() ? data.value("total", static_cast<long>(arr.size())) : static_cast<long>(arr.size());
    for (auto& it : arr) r.Items.push_back(mapItem(it));
    return r;
}

jellyfin::Detail getDetail(const std::string& guid) {
    jellyfin::Detail d;
    try {
        auto data = requestJson("GET", std::string("/v/api/v1/item/") + guid);
        if (data.is_object() && data.contains("item") && data["item"].is_object()) d = mapDetail(data["item"]);
        else d = mapDetail(data);
    } catch (...) {
        auto info = requestJson("POST", "/v/api/v1/play/info", nlohmann::json{{"item_guid", guid}});
        if (info.contains("item") && info["item"].is_object()) d = mapDetail(info["item"]);
        else d = mapDetail(info);
    }
    if (d.People.empty()) {
        try {
            d.People = fetchPeople(guid);
        } catch (const std::exception& ex) {
            brls::Logger::warning("person list failed: {}", ex.what());
        }
    }
    return d;
}

jellyfin::Result<jellyfin::Episode> listSeasons(const std::string& seriesGuid) {
    nlohmann::json data;
    try {
        data = requestJson("GET", std::string("/v/api/v1/season/list/") + seriesGuid);
    } catch (...) {
        data = requestJson("POST", "/v/api/v1/item/list", nlohmann::json{
            {"parent_guid", seriesGuid},
            {"exclude_folder", 0},
            {"sort_column", "sort_title"},
            {"sort_type", "ASC"},
        });
    }
    jellyfin::Result<jellyfin::Episode> r;
    auto arr = asArray(data);
    r.TotalRecordCount = static_cast<long>(arr.size());
    for (auto& it : arr) {
        auto item = mapItem(it);
        if (item.Type.empty()) item.Type = jellyfin::mediaTypeSeason;
        if (item.SeriesId.is_null()) item.SeriesId = seriesGuid;
        r.Items.push_back(std::move(item));
    }
    return r;
}

jellyfin::Result<jellyfin::Episode> listEpisodes(const std::string& guid) {
    auto data = requestJson("GET", std::string("/v/api/v1/episode/list/") + guid);
    jellyfin::Result<jellyfin::Episode> r;
    auto arr = asArray(data);
    r.TotalRecordCount = static_cast<long>(arr.size());
    for (auto& it : arr) {
        auto item = mapItem(it);
        item.Type = jellyfin::mediaTypeEpisode;
        if (item.SeriesId.is_null()) item.SeriesId = guid;
        r.Items.push_back(std::move(item));
    }
    return r;
}

std::string subtitleCacheDir() { return AppConfig::instance().configDir() + "/subtitles"; }

void cleanupSubtitles() {
    try {
        fs::path dir = fs::u8path(subtitleCacheDir());
        if (!fs::exists(dir)) return;
        for (auto& entry : fs::directory_iterator(dir)) {
            if (fs::is_regular_file(entry)) fs::remove(entry.path());
        }
    } catch (...) {
    }
}

static std::string downloadSubtitle(const std::string& guid, const std::string& format, const std::string& title) {
    fs::path dir = fs::u8path(subtitleCacheDir());
    fs::create_directories(dir);
    std::string safe = guid;
    for (auto& c : safe) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) c = '_';
    }
    std::string ext = format.empty() ? "srt" : format;
    for (auto& c : ext) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    fs::path path = dir / (safe + "." + ext);
    if (!fs::exists(path)) {
        const std::string body = requestRaw("GET", std::string("/v/api/v1/subtitle/dl/") + guid);
        std::ofstream of(path, std::ios::binary);
        of.write(body.data(), static_cast<std::streamsize>(body.size()));
    }
    (void)title;
    return path.u8string();
}

static void appendStream(jellyfin::Source& source, const nlohmann::json& s, const std::string& type, bool externalFile) {
    jellyfin::Stream st;
    st.Type = type;
    st.Codec = jsonString(s, {"codec_name", "codec", "format"});
    st.DisplayTitle = jsonString(s, {"title", "language", "name"}, type);
    st.Index = jsonInt(s, "index", static_cast<int>(source.MediaStreams.size()) + 1);
    st.IsExternal = externalFile || jsonInt(s, "is_external", 0) != 0;
    st.IsDefault = jsonInt(s, "is_default", 0) != 0;
    if (type == jellyfin::streamTypeVideo) source.Bitrate = jsonLong(s, "bps", static_cast<long>(source.Bitrate));
    source.MediaStreams.push_back(std::move(st));
}

PlaySession preparePlay(const std::string& itemGuid) {
    auto info = requestJson("POST", "/v/api/v1/play/info", nlohmann::json{{"item_guid", itemGuid}});
    PlaySession session;
    session.item_guid = itemGuid;
    session.media_guid = jsonString(info, {"media_guid"});
    session.video_guid = jsonString(info, {"video_guid"});
    session.audio_guid = jsonString(info, {"audio_guid"});
    session.subtitle_guid = jsonString(info, {"subtitle_guid"});
    session.title = jsonString(info.contains("item") ? info["item"] : info, {"title", "tv_title", "name"});
    if (session.media_guid.empty()) throw std::runtime_error("未返回 media_guid，无法播放");
    const std::string base = currentBase("");
    session.direct_url = base + "/v/api/v1/media/range/" + session.media_guid;
    session.source.Id = session.media_guid;
    session.source.Name = session.title;
    session.source.SupportsDirectPlay = true;
    session.source.SupportsTranscoding = true;

    try {
        auto streams = requestJson("GET", std::string("/v/api/v1/stream/list/") + itemGuid);
        if (streams.contains("video_streams"))
            for (auto& s : streams["video_streams"]) appendStream(session.source, s, jellyfin::streamTypeVideo, false);
        if (streams.contains("audio_streams"))
            for (auto& s : streams["audio_streams"]) appendStream(session.source, s, jellyfin::streamTypeAudio, false);
        if (streams.contains("subtitle_streams")) {
            for (auto& s : streams["subtitle_streams"]) {
                const bool external = jsonInt(s, "is_external", 0) != 0;
                std::string format = jsonString(s, {"format", "codec_name"});
                for (auto& c : format) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
                jellyfin::Stream st;
                st.Type = jellyfin::streamTypeSubtitle;
                st.Codec = format;
                st.DisplayTitle = jsonString(s, {"title", "language", "name"}, format);
                st.Index = jsonInt(s, "index", static_cast<int>(session.source.MediaStreams.size()) + 1);
                st.IsExternal = external;
                if (external && (format == "srt" || format == "ass" || format == "ssa" || format == "vtt" || format.empty())) {
                    try {
                        st.DeliveryUrl = downloadSubtitle(jsonString(s, {"guid", "id"}), format.empty() ? "srt" : format, st.DisplayTitle);
                    } catch (const std::exception& ex) {
                        brls::Logger::warning("subtitle download failed: {}", ex.what());
                    }
                }
                session.source.MediaStreams.push_back(std::move(st));
            }
        }
    } catch (const std::exception& ex) {
        brls::Logger::warning("stream/list failed: {}", ex.what());
    }

    try {
        nlohmann::json req = {
            {"header", {{"User-Agent", nlohmann::json::array({"trim_player"})}}},
            {"level", 1},
            {"media_guid", session.media_guid},
            {"ip", ""},
        };
        auto stream = requestJson("POST", "/v/api/v1/stream", req);
        if (stream.contains("direct_link_qualities") && stream["direct_link_qualities"].is_array()) {
            for (auto& q : stream["direct_link_qualities"]) {
                auto url = jsonString(q, {"url", "link"});
                if (!url.empty()) {
                    session.fallback_url = url;
                    break;
                }
            }
        }
        if (stream.contains("header") && stream["header"].is_object()) {
            auto cookie = stream["header"].value("Cookie", nlohmann::json::array());
            if (cookie.is_array()) {
                std::ostringstream ss;
                for (size_t i = 0; i < cookie.size(); i++) {
                    if (i) ss << "; ";
                    if (cookie[i].is_string()) ss << cookie[i].get<std::string>();
                }
                session.cookie_header = ss.str();
            }
        }
    } catch (const std::exception& ex) {
        brls::Logger::warning("stream fallback failed: {}", ex.what());
    }
    return session;
}

void loadImagePath(brls::Image* view, const std::string& path) {
    if (!view || path.empty()) return;
    const auto rel = resolveImageRel(path);
    if (rel.empty()) return;
    std::string url = rel;
    if (rel.rfind("http://", 0) != 0 && rel.rfind("https://", 0) != 0) url = currentBase("") + rel;
    Image::with(view, url, imageHeaders(rel, currentToken("")));
}

void loadPoster(brls::Image* view, const jellyfin::Item& item) {
    auto it = item.ImageTags.find(jellyfin::imageTypePrimary);
    if (it != item.ImageTags.end() && !it->second.empty()) {
        loadImagePath(view, it->second);
        return;
    }
    if (!item.Id.empty()) loadImagePath(view, item.Id);
}

}  // namespace fntv
