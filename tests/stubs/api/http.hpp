#pragma once
#include <functional>
#include <string>
#include <vector>

// Only transport is replaced. Tests compile the production fntv.cpp and models.
class HTTP {
public:
    using Header = std::vector<std::string>;
    struct Timeout { long timeout = 0; };
    static std::string request(const std::string& method, const std::string& url, const std::string& body, const Header&, Timeout) {
        return respond(method, url, body);
    }
    inline static std::function<std::string(const std::string&, const std::string&, const std::string&)> respond;
    static std::string get(const std::string& url, const Header&, Timeout) {
        return respond("GET", url, "");
    }
    static std::string post(const std::string& url, const std::string& body, const Header&, Timeout) {
        return respond("POST", url, body);
    }
};
