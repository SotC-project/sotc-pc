// SPDX-License-Identifier: GPL-2.0-or-later

// The port's own texts in the game's language, see lang.h.

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/types.h"
#include "core/sotc/lang.h"

namespace Sotc::Lang {

namespace {

struct Language {
    const char* name; // as the game's profile has it
    const char* code; // file name of the table
    Script script;
};

constexpr Script Latin{{nullptr, nullptr, nullptr}, 0};
constexpr Script Cyrillic{{nullptr, nullptr, nullptr}, 1};
constexpr Script Greek{{nullptr, nullptr, nullptr}, 2};
constexpr Script Japanese{{"YuMincho.ttc", "yumin.ttf", "msgothic.ttc"}, 3};
constexpr Script Korean{{"batang.ttc", "malgun.ttf", nullptr}, 4};
constexpr Script ChineseTraditional{{"mingliub.ttc", "msjh.ttc", nullptr}, 5};
constexpr Script ChineseSimplified{{"simsun.ttc", "msyh.ttc", nullptr}, 5};
constexpr Script Thai{{"LeelawUI.ttf", "tahoma.ttf", nullptr}, 6};
constexpr Script Arabic{{"times.ttf", "tahoma.ttf", "arial.ttf"}, 7};

// Every language the game's engine names. Which of them a copy of the game offers depends on
// the region it was sold in.
constexpr Language Languages[] = {
    {"English", "en", Latin},
    {"English Uk", "en", Latin},
    {"French", "fr", Latin},
    {"French Canadian", "fr", Latin},
    {"German", "de", Latin},
    {"Italian", "it", Latin},
    {"Spanish", "es", Latin},
    {"Latin American Spanish", "es", Latin},
    {"Portuguese", "pt", Latin},
    {"Brazilian Portuguese", "pt-br", Latin},
    {"Dutch", "nl", Latin},
    {"Polish", "pl", Latin},
    {"Russian", "ru", Cyrillic},
    {"Turkish", "tr", Latin},
    {"Swedish", "sv", Latin},
    {"Danish", "da", Latin},
    {"Norwegian", "no", Latin},
    {"Finnish", "fi", Latin},
    {"Czech", "cs", Latin},
    {"Greek", "el", Greek},
    {"Japanese", "ja", Japanese},
    {"Korean", "ko", Korean},
    {"Chinese", "zh-hant", ChineseTraditional},
    {"Simplified Chinese", "zh-hans", ChineseSimplified},
    {"Thai", "th", Thai},
    {"Arabic", "ar", Arabic},
};

/// The text language chosen in the game's settings, read from the game's profile save: the
/// name stands after a fixed mark, as one byte of length and the letters. Empty when there is
/// no profile yet (the game then starts in the console's language, English here).
std::string ProfileTextLanguage() {
    static constexpr u8 Mark[] = {0x0c, 0xea, 0xb4, 0xb1, 0xef, 0xbe, 0x0c, 0x00};
    std::error_code ec;
    const auto home = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "home";
    for (const auto& user : std::filesystem::directory_iterator(home, ec)) {
        const auto path =
            user.path() / "savedata" / "CUSA08809" / "SAVEDATA0ShadowProfile0" / "USR-DATA";
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            continue;
        }
        std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const size_t at = data.find(std::string_view(reinterpret_cast<const char*>(Mark), 8));
        if (at == std::string::npos || at + 13 >= data.size()) {
            continue;
        }
        const size_t length = static_cast<u8>(data[at + 12]);
        if (length == 0 || length > 32 || at + 13 + length > data.size()) {
            continue;
        }
        return data.substr(at + 13, length);
    }
    return {};
}

struct State {
    const Language* language = &Languages[0];
    std::string name = "English";
    std::string code = "en";
    std::string game_code = "en";
    std::unordered_map<std::string, std::string> table;
    std::unordered_map<std::string, std::string> written; // as in the file, before Visual()
    bool right_to_left = false;
};

// ---- Right-to-left scripts ----
// The overlay draws a text one letter after the other from left to right, each letter as its
// own picture. Arabic needs two things besides: its letters take another shape when they join
// their neighbours, and the line runs from the right. Both are done here, once, when the table
// is read: the letters are replaced by the joined shapes Unicode has as characters of their own
// (Arabic Presentation Forms-B, which the common Windows faces carry), and the line is put in
// the order it is seen in, so that drawing it left to right shows it right. Hebrew needs only
// the order. Latin words and numbers inside such a line keep their own direction.

std::u32string Decode(std::string_view text) {
    std::u32string out;
    for (size_t i = 0; i < text.size();) {
        const u8 lead = static_cast<u8>(text[i]);
        const size_t length = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
        if (i + length > text.size()) {
            break;
        }
        char32_t cp = length == 1 ? lead : lead & (0xff >> (length + 1));
        for (size_t k = 1; k < length; k++) {
            cp = (cp << 6) | (static_cast<u8>(text[i + k]) & 0x3f);
        }
        out += cp;
        i += length;
    }
    return out;
}

std::string Encode(const std::u32string& text) {
    std::string out;
    for (const char32_t cp : text) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xc0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xe0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        } else {
            out += static_cast<char>(0xf0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        }
    }
    return out;
}

/// How many shapes an Arabic letter has: 4 joins on both sides, 2 only to the letter before
/// it, 1 not at all; 0 for what is no Arabic letter. The shapes of U+0621 to U+064A follow one
/// another from U+FE80 in the order alone, end, start, middle.
int ArabicShapes(char32_t cp) {
    static constexpr u8 Counts[] = {
        1, 2, 2, 2, 2, 4, 2, 4, 2, 4, 4, 4, 4, 4, 2, 2, // 0621 hamza .. 0630 thal
        2, 2, 4, 4, 4, 4, 4, 4, 4, 4, 0, 0, 0, 0, 0, 0, // 0631 reh .. 063A ghain, unused
        4, 4, 4, 4, 4, 4, 4, 2, 2, 4,                   // 0641 feh .. 064A yeh (0640 below)
    };
    if (cp == 0x640) {
        return 4; // the lengthening stroke joins on both sides and has one shape
    }
    if (cp >= 0x621 && cp <= 0x63f) {
        return Counts[cp - 0x621];
    }
    if (cp >= 0x641 && cp <= 0x64a) {
        return Counts[cp - 0x641 + 32];
    }
    return 0;
}

char32_t ArabicFirstShape(char32_t cp) {
    char32_t shape = 0xfe80;
    for (char32_t c = 0x621; c < cp; c++) {
        if (c != 0x640) {
            shape += ArabicShapes(c);
        }
    }
    return shape;
}

bool ArabicMark(char32_t cp) {
    return cp >= 0x64b && cp <= 0x65f; // vowel signs: stand on a letter, join nothing
}

bool RightToLeftLetter(char32_t cp) {
    return (cp >= 0x590 && cp <= 0x8ff) || (cp >= 0xfb1d && cp <= 0xfdff) ||
           (cp >= 0xfe70 && cp <= 0xfeff);
}

bool LeftToRightLetter(char32_t cp) {
    return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
           (cp >= 0xc0 && cp < 0x590);
}

std::u32string JoinArabic(const std::u32string& text) {
    const auto neighbour = [&](size_t from, int step) -> char32_t {
        for (size_t i = from + step; i < text.size(); i += step) {
            if (!ArabicMark(text[i])) {
                return text[i];
            }
        }
        return 0;
    };
    std::u32string out;
    for (size_t i = 0; i < text.size(); i++) {
        const char32_t cp = text[i];
        const int shapes = ArabicShapes(cp);
        if (shapes == 0 || cp == 0x640) {
            out += cp;
            continue;
        }
        const bool joins_before = ArabicShapes(neighbour(i, -1)) == 4;
        // Lam followed by an alef is written as one sign.
        if (cp == 0x644) {
            size_t next = i + 1;
            while (next < text.size() && ArabicMark(text[next])) {
                ++next;
            }
            const char32_t alef = next < text.size() ? text[next] : 0;
            const char32_t ligature = alef == 0x622   ? 0xfef5
                                      : alef == 0x623 ? 0xfef7
                                      : alef == 0x625 ? 0xfef9
                                      : alef == 0x627 ? 0xfefb
                                                      : 0;
            if (ligature) {
                out += ligature + (joins_before ? 1 : 0);
                i = next;
                continue;
            }
        }
        const bool joins_after = shapes == 4 && ArabicShapes(neighbour(i, 1)) >= 2;
        const char32_t first = ArabicFirstShape(cp);
        if (shapes == 1) {
            out += first;
        } else if (shapes == 2) {
            out += first + (joins_before ? 1 : 0);
        } else {
            out += first + (joins_before && joins_after ? 3 : joins_before ? 1 : joins_after ? 2 : 0);
        }
    }
    return out;
}

/// The line in the order it is seen in: turned about as a whole, with every run of Latin
/// letters and numbers (and what stands between two of them) turned back.
std::u32string SeenOrder(std::u32string text) {
    std::reverse(text.begin(), text.end());
    for (char32_t& cp : text) {
        cp = cp == '(' ? ')' : cp == ')' ? '(' : cp == '[' ? ']' : cp == ']' ? '[' : cp;
    }
    for (size_t i = 0; i < text.size();) {
        if (!LeftToRightLetter(text[i])) {
            ++i;
            continue;
        }
        size_t end = i;
        for (size_t k = i; k < text.size() && !RightToLeftLetter(text[k]); k++) {
            if (LeftToRightLetter(text[k])) {
                end = k;
            }
        }
        std::reverse(text.begin() + i, text.begin() + end + 1);
        for (size_t k = i; k <= end; k++) {
            char32_t& cp = text[k];
            cp = cp == '(' ? ')' : cp == ')' ? '(' : cp == '[' ? ']' : cp == ']' ? '[' : cp;
        }
        i = end + 1;
    }
    return text;
}

/// A text of a right-to-left language as the overlay must draw it; other texts unchanged.
std::string Visual(const std::string& text, bool& right_to_left) {
    const std::u32string letters = Decode(text);
    if (std::none_of(letters.begin(), letters.end(), RightToLeftLetter)) {
        return text;
    }
    right_to_left = true;
    std::u32string out;
    // Line by line: each line is turned about by itself.
    size_t start = 0;
    for (size_t i = 0; i <= letters.size(); i++) {
        if (i == letters.size() || letters[i] == U'\n') {
            out += SeenOrder(JoinArabic(letters.substr(start, i - start)));
            if (i < letters.size()) {
                out += U'\n';
            }
            start = i + 1;
        }
    }
    return Encode(out);
}

std::string Unescape(std::string text) {
    for (size_t at = 0; (at = text.find("\\n", at)) != std::string::npos; ++at) {
        text.replace(at, 2, "\n");
    }
    return text;
}

const State& Get() {
    static const State state = [] {
        State s;
        // SOTC_LANG=<name> for test runs.
        const char* forced = std::getenv("SOTC_LANG");
        std::string name = forced ? forced : ProfileTextLanguage();
        for (const Language& language : Languages) {
            if (name == language.name) {
                s.language = &language;
                s.name = name;
                s.code = language.code;
            }
        }
        s.game_code = s.code;
        // A language the game does not have (Hebrew, say) can still be had for the texts of
        // this build: <user folder>/sotc_language.txt names the table to use ("he" for
        // lang/he.txt). The game's own texts stay in the language set in the game.
        {
            std::ifstream chosen(Common::FS::GetUserPath(Common::FS::PathType::UserDir) /
                                 "sotc_language.txt");
            std::string code;
            if (!forced && chosen >> code && !code.empty() &&
                code.find_first_of("/\\.:") == std::string::npos) {
                s.code = code;
                s.name = code;
            }
        }
        if (s.code != "en") {
            std::error_code ec;
            const auto path = std::filesystem::current_path(ec) / "lang" / (s.code + ".txt");
            std::ifstream file(path, std::ios::binary);
            std::string line;
            while (std::getline(file, line)) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                const size_t tab = line.find('\t');
                if (line.empty() || line[0] == '#' || tab == std::string::npos ||
                    tab + 1 >= line.size()) {
                    continue;
                }
                const std::string written = Unescape(line.substr(tab + 1));
                s.written.emplace(Unescape(line.substr(0, tab)), written);
                s.table.emplace(Unescape(line.substr(0, tab)), Visual(written, s.right_to_left));
            }
        }
        LOG_INFO(Core, "Text language in the game's profile: {}; {} translated texts ({})",
                 name.empty() ? "none" : name, s.table.size(), s.code);
        return s;
    }();
    return state;
}

} // namespace

const std::string& Name() {
    return Get().name;
}

const std::string& Code() {
    return Get().code;
}

const std::string& GameCode() {
    return Get().game_code;
}

Script ScriptInUse() {
    return Get().language->script;
}

bool RightToLeft() {
    return Get().right_to_left;
}

const std::string& Written(std::string_view english) {
    const State& state = Get();
    static const std::string none;
    const auto it = state.written.find(std::string{english});
    return it == state.written.end() ? none : it->second;
}

const std::string& Tr(std::string_view english) {
    const State& state = Get();
    // The texts are few and asked for by a handful of draws per frame; the strings handed out
    // must outlive the call, so every text asked for is kept.
    thread_local std::unordered_map<std::string, std::string> seen;
    const std::string key{english};
    if (const auto it = state.table.find(key); it != state.table.end()) {
        return it->second;
    }
    return seen.try_emplace(key, key).first->second;
}

} // namespace Sotc::Lang
