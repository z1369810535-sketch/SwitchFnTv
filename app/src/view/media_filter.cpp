#include "view/media_filter.hpp"
#include "api/fntv.hpp"

using namespace brls::literals;

MediaFilter::MediaFilter(int sort, int order, Apply apply)
    : sort(sort), order(order), initialSort(sort), initialOrder(order), apply(std::move(apply)) {
    this->inflateFromXMLRes("xml/view/media_filter.xml");
    brls::Logger::debug("MediaFilter: create");

    this->registerAction("hints/cancel"_i18n, brls::BUTTON_B, [this](...) { return this->close(); });
    this->cancel->registerClickAction([this](...) { return this->close(); });
    this->cancel->addGestureRecognizer(new brls::TapGestureRecognizer(this->cancel));

    std::vector<std::string> names;
    for (const auto& option : fntv::sortOptions()) names.push_back(brls::getStr(option.label));
    if (this->sort < 0 || this->sort >= static_cast<int>(names.size())) this->sort = 0;
    this->sortBy->init("main/media/sort_by"_i18n, names, this->sort, [this](int selected) { this->sort = selected; });

    this->sortOrder->init("main/media/order"_i18n,
        {
            "main/media/ascending"_i18n,
            "main/media/descending"_i18n,
        },
        this->order ? 1 : 0, [this](int selected) { this->order = selected; });
}

bool MediaFilter::close() {
    // Copy what the callback needs: this panel is destroyed by popActivity.
    const bool changed = this->sort != this->initialSort || this->order != this->initialOrder;
    auto callback = this->apply;
    const int s = this->sort;
    const int o = this->order;
    brls::Application::popActivity(brls::TransitionAnimation::NONE, [changed, callback, s, o]() {
        if (changed && callback) callback(s, o);
    });
    return true;
}

MediaFilter::~MediaFilter() { brls::Logger::debug("MediaFilter: delete"); }
