// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <imgui.h>

// Shared look of the PC overlay screens, after the game's own menus: a dark veil, pale serif
// text, a small triangle marking the selection, and button prompts bottom right that show only
// the input device in use.
namespace Sotc::Ui {

enum class Action { Confirm, Back, Menu, Change, Choose };

/// The serif face for overlay text (a system font, loaded once at start); the default UI font
/// when none was found.
ImFont* Serif();
void SetSerif(ImFont* font);

inline constexpr ImU32 TextBright = IM_COL32(244, 242, 232, 255);
inline constexpr ImU32 TextNormal = IM_COL32(196, 194, 184, 255);
inline constexpr ImU32 TextDim = IM_COL32(120, 119, 112, 255);

/// Ink: the page stands in the place of one of the game's own menus, on the game's picture
/// with nothing behind it, so it is written as those menus are: near-black, no shadow.
void SetInk(bool ink);
bool IsInk();
/// A window of the runtime's own (the save list) stands over the menu this frame: called by
/// it each frame it draws, so the menu's added lines and the game's lettering stay away.
void MarkDialog();
bool DialogOpen();
/// Height of the display in units of the 1080-line layout the sizes below are written in.
float Scale();
/// Darkens the whole picture behind an overlay screen.
void Veil(float opacity);
ImVec2 TextSize(float size, const std::string& text);
void Text(ImDrawList* draw, float size, ImVec2 pos, ImU32 color, const std::string& text);
void TextCentered(ImDrawList* draw, float size, float center_x, float y, ImU32 color,
                  const std::string& text);
void TextRight(ImDrawList* draw, float size, float right_x, float y, ImU32 color,
               const std::string& text);
/// The selection marker (pointing right) or a value arrow (either way).
void Triangle(ImDrawList* draw, ImVec2 center, float size, bool point_right, ImU32 color);
/// One "LABEL [icon]" prompt, right aligned at `right_x`, for the device in use. Returns the
/// height used, so prompts can be stacked.
float Prompt(ImDrawList* draw, float right_x, float y, const char* label, Action action);

} // namespace Sotc::Ui
