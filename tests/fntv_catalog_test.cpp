#include "api/fntv.hpp"
#include <iostream>
#include <stdexcept>

using json = nlohmann::json;
using Response = std::function<json(const std::string&, const std::string&, const json&)>;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void mock(const Response& response) {
    HTTP::respond = [response](const std::string& method, const std::string& url, const std::string& body) {
        return response(method, url.substr(std::string("http://test.invalid").size()),
            body.empty() ? json() : json::parse(body)).dump();
    };
}

json ok(const json& data) { return {{"code", 0}, {"data", data}}; }
json episode(const std::string& id, int number) {
    return {{"guid", id}, {"type", "Episode"}, {"episode_number", number}, {"title", id}};
}

void libraries() {
    const json rows = json::array({{{"mdb_guid", "lib"}, {"title", "TV library"}, {"category", "TV"}}});
    for (const auto& response : {ok(rows), rows, ok(json{{"items", rows}}), json{{"code", "0"}, {"data", rows}}}) {
        mock([response](auto, auto, auto) { return response; });
        const auto result = fntv::listLibraries();
        check(result.size() == 1 && result[0].Id == "lib", "library response variants");
        check(result[0].Name == "TV library", "library title");
        check(result[0].ImageTags.empty(), "missing library poster must not be fabricated from its guid");
    }
}

void totalsAndQueries() {
    for (const auto& total : {json(72), json("72"), json(nullptr)}) {
        mock([total](auto method, auto path, const json& body) {
            check(method == "POST" && path == "/v/api/v1/item/list", "item endpoint");
            check(body.at("ancestor_guid") == "lib" && body.at("page") == 2, "stable library pagination");
            check(body.at("tags").at("type") == json::array({"Movie", "TV", "Directory", "Video"}),
                "include unrecognised videos and directories");
            return ok({{"total", total}, {"items", json::array({{{"guid", "tv"}, {"type", "TV"}}})}});
        });
        const auto result = fntv::listItems("lib", 12, 12);
        check(result.Items.size() == 1 && result.StartIndex == 12, "nullable or string total must not discard items");
        check(result.TotalRecordCount == (total.is_null() ? -1 : 72), "unknown total is not zero");
    }
    mock([](auto, auto, const json& body) {
        check(body.contains("parent_guid") && !body.contains("ancestor_guid"), "folder uses direct children");
        check(!body.contains("tags"), "folder children are not hidden by a movie/TV filter");
        return ok({{"list", json::array()}, {"total", 0}});
    });
    fntv::listItems("folder", 0, 12, jellyfin::mediaTypeFolder);
}

void emptyPage() {
    int calls = 0;
    mock([&calls](auto, auto, const json& body) {
        ++calls;
        check(body.contains("ancestor_guid"), "end of list must not switch query to a different parent scope");
        return ok({{"list", json::array()}, {"total", 0}});
    });
    check(fntv::listItems("lib", 24, 12).Items.empty() && calls == 1, "empty page is terminal");
}

void missingSeasonType() {
    mock([](auto, auto path, auto) {
        check(path == "/v/api/v1/season/list/show", "season endpoint");
        return ok(json::array({
            {{"guid", "s10"}, {"season_number", 10}},
            {{"guid", "s2"}, {"season_number", "2"}},
            {{"guid", "s0"}, {"season_number", 0}},
            {{"guid", "s1"}, {"season_number", 1}, {"episode_number", 0}},
            {{"guid", "s2"}, {"season_number", 2}}}));
    });
    const auto result = fntv::listSeasons("show");
    check(result.Items.size() == 4, "deduplicate seasons");
    for (const auto& item : result.Items) {
        check(item.Type == jellyfin::mediaTypeSeason && item.SeriesId == "show", "season type and series identity");
    }
    check(result.Items[0].IndexNumber == 0 && result.Items[1].IndexNumber == 1 &&
        result.Items[2].IndexNumber == 2 && result.Items[3].IndexNumber == 10, "numeric season order including specials");
}

void multipleSeasons() {
    mock([](auto, auto path, auto) {
        if (path == "/v/api/v1/season/list/show") return ok(json::array({
            {{"guid", "s2"}, {"type", "Season"}, {"season_number", 2}},
            {{"guid", "s1"}, {"type", "Season"}, {"season_number", 1}}}));
        if (path == "/v/api/v1/episode/list/s1") return ok(json::array({episode("e12", 2), episode("e11", 1)}));
        if (path == "/v/api/v1/episode/list/s2") return ok(json::array({episode("e21", 1)}));
        throw std::runtime_error("series guid must not be used as season guid");
    });
    const auto result = fntv::listSeriesEpisodes("show");
    check(result.TotalRecordCount == 3 && result.Items[0].Id == "e11" && result.Items[2].Id == "e21",
        "all seasons included in playback order");
    check(result.Items[2].ParentIndexNumber == 2 && result.Items[2].SeriesId == "show", "parent metadata supplied");
}

void paginatedFallback() {
    int pages = 0;
    mock([&pages](auto, auto path, const json& body) {
        if (path == "/v/api/v1/episode/list/season")
            return ok({{"total", "205"}, {"list", json::array({episode("e1", 1)})}});
        check(path == "/v/api/v1/item/list" && body.at("parent_guid") == "season", "paginated child endpoint");
        const int page = body.at("page").get<int>();
        ++pages;
        auto rows = json::array();
        for (int i = (page - 1) * 100 + 1; i <= page * 100 && i <= 205; ++i) rows.push_back(episode("e" + std::to_string(i), i));
        return ok({{"total", "205"}, {"list", rows}});
    });
    const auto result = fntv::listEpisodes("season");
    check(result.Items.size() == 205 && result.Items.back().IndexNumber == 205 && pages == 3,
        "complete paginated episodes beyond the first 100");
}

void directEpisodes() {
    mock([](auto, auto path, auto) {
        if (path == "/v/api/v1/season/list/show") return ok(json::array());
        if (path == "/v/api/v1/item/list") return ok({{"total", 2}, {"list", json::array({episode("e2", 2), episode("e1", 1)})}});
        throw std::runtime_error("unexpected request");
    });
    const auto result = fntv::listSeriesEpisodes("show");
    check(result.Items.size() == 2 && result.Items[0].Id == "e1", "TV shows without season containers");
}

void invalidResponses() {
    mock([](auto, auto, auto) { return ok({{"unexpected", 42}}); });
    bool failed = false;
    try { fntv::listLibraries(); } catch (const std::exception&) { failed = true; }
    check(failed, "unknown response shape must report an error rather than an empty library");

    mock([](auto, auto path, auto) {
        if (path == "/v/api/v1/episode/list/season") return ok(json::array());
        return ok({{"total", 4}, {"list", json::array({episode("e1", 1)})}});
    });
    failed = false;
    try { fntv::listEpisodes("season"); } catch (const std::exception&) { failed = true; }
    check(failed, "repeated incomplete pages must not loop forever or claim completeness");
}

void unknownTotalFallback() {
    int calls = 0;
    mock([&calls](auto, auto path, auto) {
        if (path == "/v/api/v1/episode/list/season") return ok(json::array());
        ++calls;
        // An older server returns an unpaged list and ignores page/page_size.
        return ok({{"total", nullptr}, {"items", json::array({episode("e1", 1), episode("e2", 2)})}});
    });
    const auto result = fntv::listEpisodes("season");
    check(result.Items.size() == 2 && calls == 2, "unpaged fallback without total must terminate");
}

void originalError() {
    mock([](auto, auto path, auto) {
        if (path == "/v/api/v1/mediadb/list") return json{{"code", 403}, {"msg", "library access denied"}};
        return json{{"code", 404}, {"msg", "legacy endpoint missing"}};
    });
    try { fntv::listLibraries(); }
    catch (const std::exception& ex) {
        check(std::string(ex.what()) == "library access denied", "legacy fallback must preserve original error");
        return;
    }
    throw std::runtime_error("API failure was silently treated as success");
}

int main() {
    const std::pair<const char*, void(*)()> cases[] = {
        {"library response variants", libraries}, {"totals and library/folder queries", totalsAndQueries},
        {"end-of-list scope", emptyPage}, {"season typing and sorting", missingSeasonType},
        {"cross-season playback", multipleSeasons}, {"205-episode pagination", paginatedFallback},
        {"shows without seasons", directEpisodes}, {"invalid and repeated responses", invalidResponses},
        {"unpaged servers without totals", unknownTotalFallback}, {"preserve original API error", originalError}};
    for (const auto& test : cases) {
        try { test.second(); std::cout << "PASS " << test.first << '\n'; }
        catch (const std::exception& ex) { std::cerr << "FAIL " << test.first << ": " << ex.what() << '\n'; return 1; }
    }
}
