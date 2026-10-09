//
// Copyright 2023 dragonflylee
//

#pragma once

#include <borealis.hpp>
#include <functional>

/// Side panel to choose the sort of a library / folder grid.
/// The sort columns come from fntv::sortOptions(); the played/unplayed filters of the
/// Jellyfin version are hidden because item/list has no equivalent.
class MediaFilter : public brls::Box {
public:
    using Apply = std::function<void(int sort, int order)>;

    /// sort: index into fntv::sortOptions(); order: 0 ascending, 1 descending.
    /// apply runs after the panel closed, only when the selection changed.
    MediaFilter(int sort, int order, Apply apply);
    ~MediaFilter() override;

    bool isTranslucent() override { return true; }

private:
    BRLS_BIND(brls::Box, cancel, "filter/cancel");
    BRLS_BIND(brls::SelectorCell, sortBy, "media/sort/by");
    BRLS_BIND(brls::SelectorCell, sortOrder, "media/sort/order");
    BRLS_BIND(brls::BooleanCell, filterPlayed, "media/filter/played");
    BRLS_BIND(brls::BooleanCell, filterUnplayed, "media/filter/unplayed");

    bool close();

    int sort;
    int order;
    const int initialSort;
    const int initialOrder;
    Apply apply;
};
