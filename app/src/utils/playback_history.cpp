#include "utils/playback_history.hpp"
#include "utils/misc.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <stdexcept>

namespace fntv {
namespace {
nlohmann::json safeItem(const jellyfin::Episode& item) {
    return {{"Id", item.Id}, {"Name", item.Name}, {"Type", item.Type},
        {"ProductionYear", item.ProductionYear}, {"RunTimeTicks", item.RunTimeTicks},
        {"UserData", item.UserData}, {"SeriesId", item.SeriesId}, {"SeriesName", item.SeriesName},
        {"IndexNumber", item.IndexNumber}, {"ParentIndexNumber", item.ParentIndexNumber}};
}
}

PlaybackHistory::PlaybackHistory(std::string path) : file(std::move(path)) {
    for (const auto& candidate : {file, file + ".bak"}) {
        try {
            std::ifstream input(fs::u8path(candidate));
            if (!input) continue;
            nlohmann::json data;
            input >> data;
            if (!data.is_array()) continue;
            for (const auto& row : data) {
                try {
                    PlaybackRecord r{row.at("item").get<jellyfin::Episode>(), row.at("revision").get<int64_t>(),
                        row.value("pending", true)};
                    if (!r.item.Id.empty() && r.item.UserData.PlaybackPositionTicks >= 0) entries.push_back(r);
                } catch (...) { /* Skip only the damaged record. */ }
            }
            break;
        } catch (...) { /* Interrupted writes can be recovered from the backup. */ }
    }
    std::stable_sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.revision > b.revision; });
    if (entries.size() > 200) entries.resize(200);
}

std::optional<PlaybackRecord> PlaybackHistory::get(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& r : entries) if (r.item.Id == id) return r;
    return std::nullopt;
}

std::vector<PlaybackRecord> PlaybackHistory::records() const {
    std::lock_guard<std::mutex> lock(mutex);
    return entries;
}

int64_t PlaybackHistory::save(const jellyfin::Episode& item, double seconds, double duration, bool completed) {
    if (item.Id.empty() || !std::isfinite(seconds) || seconds < 0 || !std::isfinite(duration) || duration < 0)
        return 0;
    std::lock_guard<std::mutex> lock(mutex);
    PlaybackRecord r;
    r.item = safeItem(item).get<jellyfin::Episode>();
    // Bound corrupt media timestamps before converting to ticks.
    duration = std::min(duration, 31536000.0);
    seconds = std::min(seconds, duration > 0 ? duration : 31536000.0);
    r.item.RunTimeTicks = duration > 0 ? static_cast<uint64_t>(duration * jellyfin::PLAYTICKS) : item.RunTimeTicks;
    r.item.UserData.PlaybackPositionTicks = completed ? 0 : static_cast<int64_t>(seconds * jellyfin::PLAYTICKS);
    r.item.UserData.Played = completed;
    r.item.UserData.PlayedPercentage = completed ? 100 : (duration > 0 ? seconds * 100 / duration : 0);
    r.revision = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (!entries.empty()) r.revision = std::max(r.revision, entries.front().revision + 1);
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const auto& old) { return old.item.Id == item.Id; }), entries.end());
    entries.insert(entries.begin(), r);
    if (entries.size() > 200) entries.resize(200);
    flush();
    return r.revision;
}

void PlaybackHistory::acknowledge(const std::string& id, int64_t revision) {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto& r : entries) {
        if (r.item.Id == id && r.revision == revision) {
            r.pending = false;
            flush();
            return;
        }
    }
}

void PlaybackHistory::flush() {
    auto path = fs::u8path(file);
    fs::create_directories(path.parent_path());
    nlohmann::json data = nlohmann::json::array();
    for (const auto& r : entries) data.push_back({{"item", safeItem(r.item)}, {"revision", r.revision}, {"pending", r.pending}});
    const auto temporary = fs::u8path(file + ".tmp");
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << data.dump();
        out.flush();
        if (!out) throw std::runtime_error("无法保存播放记录，请检查 SD 卡剩余空间");
    }
    const auto backup = fs::u8path(file + ".bak");
    if (fs::exists(path)) {
        if (fs::exists(backup)) fs::remove(backup);
        fs::rename(path, backup);
    }
    fs::rename(temporary, path);
}

}  // namespace fntv
