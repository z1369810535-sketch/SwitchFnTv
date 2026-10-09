/*
    Copyright 2023 dragonflylee
*/

#pragma once

#include <borealis.hpp>

class RecyclingGrid;
class MediaFilter;

class MediaCollection : public brls::Box {
public:
    explicit MediaCollection(const std::string& itemId, const std::string& itemType = "",
        const std::string& genresId = "", const std::string& title = "");

    brls::View* getDefaultFocus() override;

    /// Called when the account changes: forget the per-session sort choices.
    static void clearPref();

private:
    BRLS_BIND(RecyclingGrid, recycler, "media/series");
    BRLS_BIND(brls::Header, header, "media/header");

    struct SortPref {
        int sort;
        int order;
    };

    void doRequest(bool refresh = false);
    void openSort();
    void updateHeader();
    bool isFolder() const;

    std::string itemId;
    std::string genresId;
    std::string itemType;
    std::string title;
    size_t pageSize;
    size_t startIndex;
    size_t requestGeneration = 0;
    long totalCount = -1;
    int sortIndex = 0;
    int orderIndex = 0;
    bool loading = false;
    bool hasMore = true;

    static SortPref libraryPref;
    static SortPref folderPref;
    static std::map<std::string, std::string> customPrefs;
};
