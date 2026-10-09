// Arabic fan translation overlay (docs/rtl-text.md): when enabled, the game's 2D language pack
// (permanent_2d_*.pack) and title logo (Layout/Title_00.szs) load from the arabic folder instead
// of the game dump. The right-to-left text support then sees the translation's font and turns on.
#pragma once
#include <string>

namespace arabic {

// Enabled by default: with no translation files present it changes nothing; the app's menu turns
// it off, and on desktop dropping the files into dir() is the whole install.
void set_enabled(bool on);
bool enabled();
// Bit 0: a permanent_2d_*.pack is in dir(); bit 1: Title_00.szs is there.
int ready();
// <save dir>/../arabic (WWHD_ARABIC_DIR overrides it).
std::string dir();

}  // namespace arabic
