#pragma once
#include "api/jellyfin/media.hpp"
namespace jellyfin {
template <typename T> struct Result {
    std::vector<T> Items;
    long TotalRecordCount = 0;
    long StartIndex = 0;
};
}
