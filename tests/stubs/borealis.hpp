#pragma once
#include <functional>
namespace brls {
class Image {};
struct Logger {
    template <typename... Args> static void warning(Args&&...) {}
};
inline void async(const std::function<void()>& work) { work(); }
inline void sync(const std::function<void()>& work) { work(); }
}
