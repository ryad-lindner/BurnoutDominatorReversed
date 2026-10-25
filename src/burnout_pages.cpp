#include "burnout_pages.h"

#include <cctype>
#include <cstring>

namespace burnout {
namespace {
bool nameIs(const uint8_t* name, size_t room, const char* page) {
    for (size_t i = 0;; ++i) {
        if (i >= room || std::tolower(name[i]) != page[i]) return false;
        if (page[i] == 0) return true;
    }
}
} // namespace

Page classifyPage(const uint8_t* name, size_t room) {
    static constexpr struct { const char* name; Page page; } kPages[] = {
        {"dd_profile_loadsave", Page::ProfileLoadSave},
        {"su_frequency", Page::Frequency},
        {"su_movie", Page::Movie},
        {"su_movie2", Page::Movie2},
        {"su_intro", Page::Intro}};
    for (const auto& entry : kPages)
        if (nameIs(name, room, entry.name)) return entry.page;
    return Page::Other;
}

bool IntroSkip::onGoToPage(Page page, uint8_t* name) {
    if (m_on && isSkippedPage(page)) return onFrequencySkip(name);
    if (page != Page::ProfileLoadSave && page != Page::Frequency) m_on = false;
    return false;
}

bool IntroSkip::onFrequencySkip(uint8_t* name) {
    if (!m_on) return false;
    m_on = false;
    std::memcpy(name, "SU_Intro", 9);
    return true;
}
} // namespace burnout
