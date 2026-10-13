// SPDX-License-Identifier: GPL-2.0-or-later

// The port's own texts in the game's language, see lang.h.

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
    {"Arabic", "ar", Latin}, // not offered by this build: its letters need joining and a
                             // right-to-left layout, which the overlay text cannot do
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
    std::unordered_map<std::string, std::string> table;
};

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
                s.table.emplace(Unescape(line.substr(0, tab)), Unescape(line.substr(tab + 1)));
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

Script ScriptInUse() {
    return Get().language->script;
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
