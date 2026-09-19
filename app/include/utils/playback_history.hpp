#pragma once

#include <api/jellyfin/media.hpp>
#include <mutex>
#include <optional>

namespace fntv {

struct PlaybackRecord {
    jellyfin::Episode item;
    int64_t revision = 0;
    bool pending = true;
};

// One file per server/user. Media URLs, headers and credentials are never stored.
class PlaybackHistory {
public:
    explicit PlaybackHistory(std::string file);
    std::optional<PlaybackRecord> get(const std::string& id) const;
    std::vector<PlaybackRecord> records() const;
    int64_t save(const jellyfin::Episode& item, double seconds, double duration, bool completed);
    void acknowledge(const std::string& id, int64_t revision);
private:
    void flush();
    std::string file;
    mutable std::mutex mutex;
    std::vector<PlaybackRecord> entries;
};

}  // namespace fntv
