#pragma once

#include <cstddef>
#include <cstdint>

namespace burnout {
// The start-up pages the runner treats specially, by the name the game passes to the page
// manager's go-to-page (0x1838A8).
enum class Page {
    Other,
    ProfileLoadSave, // DD_Profile_LoadSave: the boot's first page
    Frequency,       // SU_Frequency
    Movie,           // SU_Movie
    Movie2,          // SU_Movie2
    Intro,           // SU_Intro
};

// The page a NUL-terminated name stands for, ignoring case; `room` bounds the bytes read.
Page classifyPage(const uint8_t* name, size_t room);

// SU_Movie (logos) and SU_Movie2 (intro movie): the pages skip_intro leaves out. SU_Intro is kept:
// it starts the start screen's music and background stream (E023 in docs/FINDINGS.md).
inline bool isSkippedPage(Page page) { return page == Page::Movie || page == Page::Movie2; }

// skip_intro over a run. The go-to-page then gets "SU_Intro" in place of SU_Movie or SU_Movie2,
// or of the SU_Movie that the 50/60 Hz skip would give SU_Frequency, once. The name is rewritten
// in the game's own buffer, which holds at least 9 bytes, "SU_Intro" and its NUL (the executable
// has no "SU_Intro" literal to point at: the names come from the scripts). That buffer is the
// script's string, so the new name stays until the scripts are loaded again, as SU_Frequency's
// "SU_Movie" of the 50/60 Hz skip does. The skip ends with its redirect, or when the game asks
// for a page that is not one of the boot's before the videos (DD_Profile_LoadSave, SU_Frequency):
// a game restored from a save state, or the start screen's attract mode, never meets it.
class IntroSkip {
public:
    explicit IntroSkip(bool on) : m_on(on) {}
    // The game asks for `page`, named in `name`: true if the name became "SU_Intro".
    bool onGoToPage(Page page, uint8_t* name);
    // The 50/60 Hz skip is about to give SU_Frequency's go-to-page `name` SU_Movie: true if it
    // became "SU_Intro" instead.
    bool onFrequencySkip(uint8_t* name);

private:
    bool m_on;
};
} // namespace burnout
