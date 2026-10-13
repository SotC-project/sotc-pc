// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>

// The texts this build adds to the game (menu lines, settings pages, free play, achievements,
// hints) in the language the game's own text is set to.
//
// The texts are written in English in the code and looked up, whole, in a table for the
// language: <program folder>/lang/<code>.txt, one text per line as
//     English text<TAB>translation
// in UTF-8. A text the table does not have stays English. The language is the one chosen under
// the game's Settings > Language > Text, read from the game's profile when the program starts.
namespace Sotc::Lang {

/// The language in effect, as the name the game's profile has for it ("English", "French", ...).
const std::string& Name();
/// The file name of its table without the ending ("fr", "de", ...); "en" for English.
const std::string& Code();
/// The text in the language in effect; the text itself when there is no translation.
const std::string& Tr(std::string_view english);
/// The font files of the system that have the letters of the language in effect, best first;
/// empty for the languages the Latin serif face covers.
struct Script {
    const char* fonts[3];
    int ranges; // 0 Latin, 1 Cyrillic, 2 Greek, 3 Japanese, 4 Korean, 5 Chinese, 6 Thai
};
Script ScriptInUse();

} // namespace Sotc::Lang
