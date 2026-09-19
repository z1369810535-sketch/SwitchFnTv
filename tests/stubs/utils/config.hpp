#pragma once
#include <string>
class AppConfig {
public:
    static AppConfig& instance() { static AppConfig config; return config; }
    inline static std::string url = "http://test.invalid";
    inline static std::string user = "test-user";
    inline static std::string directory = "build-tests/config";
    std::string getUrl() const { return url; }
    std::string getToken() const { return "test-token"; }
    std::string getUserId() const { return user; }
    std::string configDir() const { return directory; }
};
