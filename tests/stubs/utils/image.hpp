#pragma once
#include <borealis.hpp>
#include "api/http.hpp"
struct Image {
    static void with(brls::Image*, const std::string&, const HTTP::Header&) {}
};
