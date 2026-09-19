#pragma once
#include <functional>
#include <vector>
namespace brls {
class Image {};
struct Logger {
    template <typename... Args> static void warning(Args&&...) {}
};
inline bool deferAsync = false;
inline std::vector<std::function<void()>> jobs;
inline void async(const std::function<void()>& work) { if (deferAsync) jobs.push_back(work); else work(); }
inline void sync(const std::function<void()>& work) { work(); }
}
