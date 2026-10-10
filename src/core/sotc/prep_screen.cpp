// SPDX-License-Identifier: GPL-2.0-or-later

// Shader preparation screen, see prep_screen.h.

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>

#include <SDL3/SDL.h>

#include "core/sotc/prep_screen.h"

#ifdef _WIN32
#include <windows.h>

#include <xinput.h>
#pragma comment(lib, "xinput.lib")
#endif

namespace Sotc::PrepScreen {

std::string ProgramSignature() {
    std::filesystem::path program;
#ifdef _WIN32
    wchar_t buffer[1024]{};
    GetModuleFileNameW(nullptr, buffer, 1023);
    program = buffer;
#endif
    std::error_code ec;
    const auto size = std::filesystem::file_size(program, ec);
    const auto time = std::filesystem::last_write_time(program, ec).time_since_epoch().count();
    return "program " + std::to_string(ec ? 0 : size) + " " + std::to_string(time);
}

#ifdef _WIN32

namespace {

constexpr wchar_t ClassName[] = L"SotcPrepScreen";

constexpr COLORREF Ink = RGB(236, 233, 222);
constexpr COLORREF DimInk = RGB(150, 148, 138);
constexpr COLORREF Stone = RGB(120, 124, 108);
constexpr COLORREF Slot = RGB(38, 39, 35);
constexpr COLORREF Moss = RGB(104, 138, 66);
constexpr COLORREF Black = RGB(6, 6, 5);

// The serif face the in-game menus use: the first of these that is installed.
const wchar_t* SerifFace() {
    static const wchar_t* face = [] {
        static constexpr std::array<std::pair<const char*, const wchar_t*>, 4> Faces{{
            {"GARA.TTF", L"Garamond"},
            {"pala.ttf", L"Palatino Linotype"},
            {"BKANT.TTF", L"Book Antiqua"},
            {"georgia.ttf", L"Georgia"},
        }};
        std::error_code ec;
        for (const auto& [file, name] : Faces) {
            if (std::filesystem::exists(std::filesystem::path("C:\\Windows\\Fonts") / file, ec)) {
                return name;
            }
        }
        return L"Times New Roman";
    }();
    return face;
}

} // namespace

struct Screen::Impl {
    HWND parent{};
    HWND hwnd{};
    HDC mem{};
    HBITMAP bitmap{};
    HGDIOBJ old_bitmap{};
    int width{};
    int height{};
    std::map<int, HFONT> fonts;
    bool clicked{};
    POINT click{};

    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint;
            const HDC dc = BeginPaint(hwnd, &paint);
            if (self && self->mem) {
                BitBlt(dc, 0, 0, self->width, self->height, self->mem, 0, 0, SRCCOPY);
            }
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_LBUTTONDOWN:
            if (self) {
                self->click = {static_cast<short>(LOWORD(lparam)),
                               static_cast<short>(HIWORD(lparam))};
                self->clicked = true;
            }
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wparam, lparam);
        }
    }

    float Scale() const {
        return static_cast<float>(height) / 1080.0f;
    }

    int Px(float value) const {
        return std::max(1, static_cast<int>(value * Scale() + 0.5f));
    }

    HFONT Font(int size) {
        auto& font = fonts[size];
        if (!font) {
            font = CreateFontW(-size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                               DEFAULT_PITCH | FF_ROMAN, SerifFace());
        }
        return font;
    }

    /// Follows the game window's size and makes the picture to draw into match.
    void Fit() {
        RECT client{};
        GetClientRect(parent, &client);
        const int w = std::max<int>(client.right, 1), h = std::max<int>(client.bottom, 1);
        if (w == width && h == height && mem) {
            return;
        }
        width = w;
        height = h;
        MoveWindow(hwnd, 0, 0, w, h, FALSE);
        const HDC dc = GetDC(hwnd);
        if (!mem) {
            mem = CreateCompatibleDC(dc);
        } else {
            SelectObject(mem, old_bitmap);
            DeleteObject(bitmap);
        }
        bitmap = CreateCompatibleBitmap(dc, w, h);
        old_bitmap = SelectObject(mem, bitmap);
        ReleaseDC(hwnd, dc);
        SetBkMode(mem, TRANSPARENT);
        for (auto& [size, font] : fonts) {
            DeleteObject(font);
        }
        fonts.clear();
    }

    void Fill(int x, int y, int w, int h, COLORREF color) {
        const RECT rect{x, y, x + w, y + h};
        const HBRUSH brush = CreateSolidBrush(color);
        FillRect(mem, &rect, brush);
        DeleteObject(brush);
    }

    int TextWidth(const std::wstring& text, int size) {
        SelectObject(mem, Font(size));
        SIZE extent{};
        GetTextExtentPoint32W(mem, text.c_str(), static_cast<int>(text.size()), &extent);
        return extent.cx;
    }

    /// Text with its left edge at x and its top at y.
    void Text(const std::wstring& text, int x, int y, int size, COLORREF color) {
        SelectObject(mem, Font(size));
        SetTextColor(mem, color);
        TextOutW(mem, x, y, text.c_str(), static_cast<int>(text.size()));
    }

    void Centered(const std::wstring& text, int y, int size, COLORREF color) {
        Text(text, (width - TextWidth(text, size)) / 2, y, size, color);
    }

    void Show() {
        const HDC dc = GetDC(hwnd);
        BitBlt(dc, 0, 0, width, height, mem, 0, 0, SRCCOPY);
        ReleaseDC(hwnd, dc);
    }

    /// One answer of the prompt: a key cap, then what it does. Returns the area it covers.
    RECT Option(int x, int y, const std::wstring& key, const std::wstring& label, bool selected,
                bool draw) {
        const int size = Px(40), cap_size = Px(28), cap_h = Px(46), pad = Px(14), gap = Px(16);
        const int cap_w = std::max(cap_h, TextWidth(key, cap_size) + pad * 2);
        const int label_w = TextWidth(label, size);
        const RECT area{x - pad, y - pad, x + cap_w + gap + label_w + pad, y + cap_h + pad};
        if (draw) {
            const COLORREF ink = selected ? Ink : DimInk;
            const HPEN pen = CreatePen(PS_SOLID, std::max(1, Px(2)), ink);
            const HGDIOBJ old_pen = SelectObject(mem, pen);
            const HGDIOBJ old_brush = SelectObject(mem, GetStockObject(NULL_BRUSH));
            RoundRect(mem, x, y, x + cap_w, y + cap_h, Px(12), Px(12));
            SelectObject(mem, old_brush);
            SelectObject(mem, old_pen);
            DeleteObject(pen);
            Text(key, x + (cap_w - TextWidth(key, cap_size)) / 2, y + (cap_h - cap_size) / 2 - Px(2),
                 cap_size, ink);
            Text(label, x + cap_w + gap, y + (cap_h - size) / 2 - Px(3), size, ink);
            if (selected) {
                Fill(x + cap_w + gap, y + cap_h + Px(4), label_w, std::max(2, Px(3)), Moss);
            }
        }
        return area;
    }
};

Screen::Screen() {
    if (std::getenv("SOTC_HEADLESS")) {
        return;
    }
    int count = 0;
    SDL_Window** windows = SDL_GetWindows(&count);
    HWND parent{};
    for (int i = 0; windows && i < count; i++) {
        if (SDL_GetWindowFlags(windows[i]) & SDL_WINDOW_HIDDEN) {
            continue;
        }
        parent = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(windows[i]),
                                                          SDL_PROP_WINDOW_WIN32_HWND_POINTER,
                                                          nullptr));
        break;
    }
    SDL_free(windows);
    if (!parent) {
        return;
    }
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = Impl::Proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    window_class.lpszClassName = ClassName;
    RegisterClassW(&window_class);
    const HWND hwnd = CreateWindowExW(0, ClassName, L"", WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, parent,
                                      nullptr, window_class.hInstance, nullptr);
    if (!hwnd) {
        return;
    }
    impl = std::make_unique<Impl>();
    impl->parent = parent;
    impl->hwnd = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl.get()));
    impl->Fit();
    impl->Fill(0, 0, impl->width, impl->height, Black);
    impl->Show();
}

Screen::~Screen() {
    if (!impl) {
        return;
    }
    SetWindowLongPtrW(impl->hwnd, GWLP_USERDATA, 0);
    DestroyWindow(impl->hwnd);
    if (impl->mem) {
        SelectObject(impl->mem, impl->old_bitmap);
        DeleteObject(impl->bitmap);
        DeleteDC(impl->mem);
    }
    for (auto& [size, font] : impl->fonts) {
        DeleteObject(font);
    }
    InvalidateRect(impl->parent, nullptr, TRUE);
}

Choice Screen::Ask() {
    if (!impl) {
        return Choice::Prepare;
    }
    auto& s = *impl;
    enum Input { Confirm, Cancel, Left, Right, NumInputs };
    // Keys held when the prompt appears (Enter that started the game) must be let go first.
    std::array<bool, NumInputs> held;
    held.fill(true);
    int selected = 0;
    bool pad_present = false;
    bool dirty = true;
    std::array<RECT, 2> areas{};

    while (true) {
        SDL_PumpEvents();
        if (SDL_HasEvent(SDL_EVENT_QUIT)) {
            return Choice::Skip;
        }
        std::array<bool, NumInputs> down{};
        if (GetForegroundWindow() == s.parent) {
            const auto key = [](int code) { return (GetAsyncKeyState(code) & 0x8000) != 0; };
            down[Confirm] = key(VK_RETURN) || key(VK_SPACE);
            down[Cancel] = key(VK_ESCAPE);
            down[Left] = key(VK_LEFT) || key('A');
            down[Right] = key(VK_RIGHT) || key('D');
        }
        bool pad_now = false;
        for (DWORD index = 0; index < XUSER_MAX_COUNT; index++) {
            XINPUT_STATE state{};
            if (XInputGetState(index, &state) != ERROR_SUCCESS) {
                continue;
            }
            pad_now = true;
            const WORD buttons = state.Gamepad.wButtons;
            down[Confirm] |= (buttons & XINPUT_GAMEPAD_A) != 0;
            down[Cancel] |= (buttons & XINPUT_GAMEPAD_B) != 0;
            down[Left] |= (buttons & XINPUT_GAMEPAD_DPAD_LEFT) != 0 ||
                          state.Gamepad.sThumbLX < -16000;
            down[Right] |= (buttons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0 ||
                           state.Gamepad.sThumbLX > 16000;
        }
        if (pad_now != pad_present) {
            pad_present = pad_now;
            dirty = true;
        }
        const auto pressed = [&](Input input) { return down[input] && !held[input]; };
        if (pressed(Left) && selected != 0) {
            selected = 0;
            dirty = true;
        }
        if (pressed(Right) && selected != 1) {
            selected = 1;
            dirty = true;
        }
        if (pressed(Cancel)) {
            return Choice::Skip;
        }
        if (pressed(Confirm)) {
            return selected == 0 ? Choice::Prepare : Choice::Skip;
        }
        held = down;
        if (s.clicked) {
            s.clicked = false;
            for (int i = 0; i < 2; i++) {
                if (PtInRect(&areas[i], s.click)) {
                    return i == 0 ? Choice::Prepare : Choice::Skip;
                }
            }
        }

        const int old_width = s.width, old_height = s.height;
        s.Fit();
        if (dirty || s.width != old_width || s.height != old_height) {
            dirty = false;
            s.Fill(0, 0, s.width, s.height, Black);
            int y = static_cast<int>(s.height * 0.30f);
            s.Centered(L"Prepare the shaders before playing?", y, s.Px(68), Ink);
            y += s.Px(120);
            s.Centered(L"Recommended. It takes up to a minute, once, and the game then runs smoothly.", y,
                       s.Px(34), DimInk);
            y += s.Px(52);
            s.Centered(L"If skipped, the game starts at once and pauses briefly whenever something "
                       L"new comes on screen.",
                       y, s.Px(34), DimInk);
            y += s.Px(52);
            s.Centered(L"You will be asked again at the next start.", y, s.Px(34), DimInk);
            y += s.Px(130);
            // Only the prompts of the device in use are shown.
            const std::wstring keys[2] = {pad_present ? L"A" : L"Enter", pad_present ? L"B" : L"Esc"};
            const std::wstring labels[2] = {L"Prepare", L"Skip"};
            const int between = s.Px(120);
            int total = between;
            for (int i = 0; i < 2; i++) {
                const RECT area = s.Option(0, 0, keys[i], labels[i], false, false);
                total += area.right - area.left - s.Px(28);
            }
            int x = (s.width - total) / 2;
            for (int i = 0; i < 2; i++) {
                areas[i] = s.Option(x, y, keys[i], labels[i], selected == i, true);
                x += areas[i].right - areas[i].left - s.Px(28) + between;
            }
            s.Show();
        }
        Sleep(8);
    }
}

void Screen::Progress(u32 done, u32 total, bool first_time) {
    // Keep answering the system, or it marks the window as not responding.
    SDL_PumpEvents();
    if (!impl) {
        return;
    }
    auto& s = *impl;
    s.Fit();
    total = std::max(total, 1u);
    done = std::min(done, total);
    s.Fill(0, 0, s.width, s.height, Black);
    int y = static_cast<int>(s.height * 0.38f);
    s.Centered(L"Preparing shaders", y, s.Px(68), Ink);
    y += s.Px(130);
    // A slot carved in stone, moss filling it.
    const int w = static_cast<int>(s.width * 0.42f), h = s.Px(14), x = (s.width - w) / 2;
    const int edge = std::max(1, s.Px(2));
    s.Fill(x - edge, y - edge, w + edge * 2, h + edge * 2, Stone);
    s.Fill(x, y, w, h, Slot);
    s.Fill(x, y, static_cast<int>(static_cast<u64>(w) * done / total), h, Moss);
    y += s.Px(44);
    s.Centered(std::to_wstring(done * 100 / total) + L"%", y, s.Px(36), DimInk);
    if (first_time) {
        y += s.Px(110);
        s.Centered(L"Needed once after an update of the game or of the graphics driver. Later "
                   L"starts take a few seconds.",
                   y, s.Px(30), DimInk);
    }
    s.Show();
}

#else

struct Screen::Impl {};

Screen::Screen() = default;
Screen::~Screen() = default;

Choice Screen::Ask() {
    return Choice::Prepare;
}

void Screen::Progress(u32, u32, bool) {
    SDL_PumpEvents();
}

#endif

} // namespace Sotc::PrepScreen
