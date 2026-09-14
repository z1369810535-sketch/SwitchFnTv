#pragma once
#include <string>
class AppConfig {
public:
    static AppConfig& instance() { static AppConfig config; return config; }
    std::string getUrl() const { return "http://test.invalid"; }
    std::string getToken() const { return "test-token"; }
    std::string configDir() const { return "build-tests/config"; }
};
