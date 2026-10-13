// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <SDL3/SDL_timer.h>
#include <algorithm>
#include <cmath>

#include "common/types.h"
#include "core/sotc/lang.h"
#include "core/sotc/ui.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Sotc::Ui {

namespace {

ImFont* serif = nullptr;

using VideoCore::PromptSet;

/// A keyboard key: a rounded cap with its name.
float Keycap(ImDrawList* draw, float right_x, float y, float height, const char* name) {
    const float text_size = height * 0.62f;
    const float width = std::max(height, TextSize(text_size, name).x + height * 0.6f);
    const ImVec2 min{right_x - width, y};
    const ImVec2 max{right_x, y + height};
    draw->AddRectFilled(min, max, IM_COL32(236, 234, 224, 255), height * 0.22f);
    draw->AddRect(min, max, IM_COL32(60, 60, 56, 255), height * 0.22f, 0, 1.5f);
    const ImVec2 size = TextSize(text_size, name);
    draw->AddText(Serif(), text_size,
                  {min.x + (width - size.x) * 0.5f, y + (height - size.y) * 0.5f},
                  IM_COL32(24, 24, 22, 255), name);
    return width;
}

/// A round pad button. `symbol`: 'x' cross, 'o' circle, or a letter drawn as text.
float PadButton(ImDrawList* draw, float right_x, float y, float height, char symbol, ImU32 ring,
                ImU32 fill, ImU32 ink) {
    const float r = height * 0.5f;
    const ImVec2 c{right_x - r, y + r};
    draw->AddCircleFilled(c, r, fill, 32);
    draw->AddCircle(c, r, ring, 32, 1.5f);
    const float k = r * 0.42f;
    if (symbol == 'x') {
        draw->AddLine({c.x - k, c.y - k}, {c.x + k, c.y + k}, ink, 2.0f);
        draw->AddLine({c.x - k, c.y + k}, {c.x + k, c.y - k}, ink, 2.0f);
    } else if (symbol == 'o') {
        draw->AddCircle(c, k * 1.15f, ink, 24, 2.0f);
    } else {
        const char text[2]{symbol, 0};
        const float size = height * 0.7f;
        const ImVec2 extent = TextSize(size, text);
        draw->AddText(Serif(), size, {c.x - extent.x * 0.5f, c.y - extent.y * 0.5f}, ink, text);
    }
    return height;
}

/// A labelled pad control that has no simple symbol (stick clicks, d-pad).
float PadLabel(ImDrawList* draw, float right_x, float y, float height, const char* name) {
    const float text_size = height * 0.58f;
    const float width = TextSize(text_size, name).x + height * 0.7f;
    const ImVec2 min{right_x - width, y};
    const ImVec2 max{right_x, y + height};
    draw->AddRectFilled(min, max, IM_COL32(40, 40, 38, 255), height * 0.5f);
    draw->AddRect(min, max, IM_COL32(200, 198, 188, 255), height * 0.5f, 0, 1.5f);
    const ImVec2 size = TextSize(text_size, name);
    draw->AddText(Serif(), text_size,
                  {min.x + (width - size.x) * 0.5f, y + (height - size.y) * 0.5f}, TextBright,
                  Lang::Tr(name).c_str());
    return width;
}

float Icon(ImDrawList* draw, float right_x, float y, float height, Action action) {
    const PromptSet set = VideoCore::ActivePromptSet();
    if (set == PromptSet::Keyboard) {
        switch (action) {
        case Action::Confirm:
            return Keycap(draw, right_x, y, height, "Enter");
        case Action::Back:
            return Keycap(draw, right_x, y, height, "Esc");
        case Action::Menu:
            return Keycap(draw, right_x, y, height, "F2");
        case Action::Change:
            return Keycap(draw, right_x, y, height, "A  D");
        default:
            return Keycap(draw, right_x, y, height, "W  S");
        }
    }
    const bool xbox = set == PromptSet::Xbox;
    switch (action) {
    case Action::Confirm:
        return xbox ? PadButton(draw, right_x, y, height, 'A', IM_COL32(20, 20, 20, 255),
                                IM_COL32(96, 160, 72, 255), IM_COL32_WHITE)
                    : PadButton(draw, right_x, y, height, 'x', IM_COL32(200, 198, 188, 255),
                                IM_COL32(34, 36, 44, 255), IM_COL32(124, 160, 226, 255));
    case Action::Back:
        return xbox ? PadButton(draw, right_x, y, height, 'B', IM_COL32(20, 20, 20, 255),
                                IM_COL32(196, 70, 60, 255), IM_COL32_WHITE)
                    : PadButton(draw, right_x, y, height, 'o', IM_COL32(200, 198, 188, 255),
                                IM_COL32(34, 36, 44, 255), IM_COL32(226, 110, 100, 255));
    case Action::Menu:
        return PadLabel(draw, right_x, y, height, xbox ? "LS + RS" : "L3 + R3");
    case Action::Change:
        return PadLabel(draw, right_x, y, height, "D-pad left / right");
    default:
        return PadLabel(draw, right_x, y, height, "D-pad up / down");
    }
}

} // namespace

ImFont* Serif() {
    return serif ? serif : ImGui::GetFont();
}

void SetSerif(ImFont* font) {
    serif = font;
}

namespace {
std::atomic<u64> dialog_ms{0};
} // namespace

// By the clock, not the frame count: the question is also asked from the render thread.
void MarkDialog() {
    dialog_ms = SDL_GetTicks() + 1;
}

bool DialogOpen() {
    const u64 marked = dialog_ms.load(std::memory_order_relaxed);
    return marked != 0 && SDL_GetTicks() + 1 - marked < 120;
}

float Scale() {
    return ImGui::GetIO().DisplaySize.y / 1080.0f;
}

void Veil(float opacity) {
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    // On the foreground list, ahead of the text drawn after it: the background list sits
    // behind the game picture here.
    ImGui::GetForegroundDrawList()->AddRectFilled(
        {0.0f, 0.0f}, size, IM_COL32(0, 0, 0, static_cast<int>(255.0f * opacity)));
}

namespace {
bool ink_mode = false;

ImU32 Inked(ImU32 color) {
    if (!ink_mode) {
        return color;
    }
    if (color == TextBright || color == TextNormal) {
        return IM_COL32(22, 22, 20, 255);
    }
    if (color == TextDim) {
        return IM_COL32(22, 22, 20, 110);
    }
    return color;
}
} // namespace

void SetInk(bool ink) {
    ink_mode = ink;
}

bool IsInk() {
    return ink_mode;
}

// Every text of the overlay screens passes through these two, so this is where it is put into
// the game's language (see lang.h). A text that is already translated, a name or a number has
// no entry in the table and stays as it is.
ImVec2 TextSize(float size, const std::string& text) {
    return Serif()->CalcTextSizeA(size, FLT_MAX, 0.0f, Lang::Tr(text).c_str());
}

void Text(ImDrawList* draw, float size, ImVec2 pos, ImU32 color, const std::string& text) {
    const std::string& shown = Lang::Tr(text);
    if (!ink_mode) {
        draw->AddText(Serif(), size, {pos.x + 1.5f, pos.y + 1.5f}, IM_COL32(0, 0, 0, 190),
                      shown.c_str());
    }
    draw->AddText(Serif(), size, pos, Inked(color), shown.c_str());
}

void TextCentered(ImDrawList* draw, float size, float center_x, float y, ImU32 color,
                  const std::string& text) {
    Text(draw, size, {center_x - TextSize(size, text).x * 0.5f, y}, color, text);
}

void TextRight(ImDrawList* draw, float size, float right_x, float y, ImU32 color,
               const std::string& text) {
    Text(draw, size, {right_x - TextSize(size, text).x, y}, color, text);
}

void Triangle(ImDrawList* draw, ImVec2 center, float size, bool point_right, ImU32 color) {
    const float d = point_right ? 1.0f : -1.0f;
    draw->AddTriangleFilled({center.x + d * size * 0.55f, center.y},
                            {center.x - d * size * 0.45f, center.y - size * 0.6f},
                            {center.x - d * size * 0.45f, center.y + size * 0.6f}, Inked(color));
}

float Prompt(ImDrawList* draw, float right_x, float y, const char* label, Action action) {
    const float scale = Scale();
    const float height = 34.0f * scale;
    const float width = Icon(draw, right_x, y, height, action);
    const float text_size = 24.0f * scale;
    TextRight(draw, text_size, right_x - width - 12.0f * scale,
              y + (height - TextSize(text_size, label).y) * 0.5f, TextBright, label);
    return height + 10.0f * scale;
}

} // namespace Sotc::Ui
