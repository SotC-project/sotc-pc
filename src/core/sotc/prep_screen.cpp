// SPDX-License-Identifier: GPL-2.0-or-later

// Shader preparation screen, see prep_screen.h.

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "core/sotc/prep_screen.h"

#ifdef _WIN32
#include <windows.h>

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

    /// How the button of an answer is drawn: a key cap with its name, a round pad button with
    /// a letter, or a PlayStation cross or circle.
    enum class Button { Key, Round, Cross, Circle };

    /// One answer of the prompt: its button, then what it does. Returns the area it covers.
    RECT Option(int x, int y, Button button, const std::wstring& key, const std::wstring& label,
                bool selected, bool draw) {
        const int size = Px(40), cap_size = Px(28), cap_h = Px(46), pad = Px(14), gap = Px(16);
        const int cap_w = button == Button::Key
                              ? std::max(cap_h, TextWidth(key, cap_size) + pad * 2)
                              : cap_h;
        const int label_w = TextWidth(label, size);
        const RECT area{x - pad, y - pad, x + cap_w + gap + label_w + pad, y + cap_h + pad};
        if (draw) {
            const COLORREF ink = selected ? Ink : DimInk;
            const HPEN pen = CreatePen(PS_SOLID, std::max(1, Px(2)), ink);
            const HGDIOBJ old_pen = SelectObject(mem, pen);
            const HGDIOBJ old_brush = SelectObject(mem, GetStockObject(NULL_BRUSH));
            if (button == Button::Key) {
                RoundRect(mem, x, y, x + cap_w, y + cap_h, Px(12), Px(12));
            } else {
                Ellipse(mem, x, y, x + cap_w, y + cap_h);
            }
            const int inset = cap_h * 30 / 100;
            if (button == Button::Cross) {
                MoveToEx(mem, x + inset, y + inset, nullptr);
                LineTo(mem, x + cap_w - inset, y + cap_h - inset);
                MoveToEx(mem, x + cap_w - inset, y + inset, nullptr);
                LineTo(mem, x + inset, y + cap_h - inset);
            } else if (button == Button::Circle) {
                Ellipse(mem, x + inset, y + inset, x + cap_w - inset, y + cap_h - inset);
            }
            SelectObject(mem, old_brush);
            SelectObject(mem, old_pen);
            DeleteObject(pen);
            if (button == Button::Key || button == Button::Round) {
                Text(key, x + (cap_w - TextWidth(key, cap_size)) / 2,
                     y + (cap_h - cap_size) / 2 - Px(2), cap_size, ink);
            }
            // The chosen answer is the bright one.
            Text(label, x + cap_w + gap, y + (cap_h - size) / 2 - Px(3), size, ink);
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
    bool dirty = true;
    std::array<RECT, 2> areas{};

    // Controllers of every kind, through the same library the game uses for them. Those opened
    // here are closed again when the prompt is answered.
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    struct Pads {
        std::vector<SDL_Gamepad*> opened;
        ~Pads() {
            for (SDL_Gamepad* pad : opened) {
                SDL_CloseGamepad(pad);
            }
        }
    } pads;
    // The buttons shown are those of the device used last: keyboard keys, or the pictures of
    // an Xbox-type or a PlayStation controller.
    enum class Device { Keyboard, Xbox, PlayStation };
    const auto device_of = [](SDL_Gamepad* pad) {
        switch (SDL_GetGamepadType(pad)) {
        case SDL_GAMEPAD_TYPE_PS3:
        case SDL_GAMEPAD_TYPE_PS4:
        case SDL_GAMEPAD_TYPE_PS5:
            return Device::PlayStation;
        default:
            return Device::Xbox;
        }
    };
    Device device = Device::Keyboard;
    bool first_pass = true;
    bool any_key_held = true;
    bool any_pad_held = true;

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
            down[Left] = key(VK_UP) || key('W') || key(VK_LEFT) || key('A');
            down[Right] = key(VK_DOWN) || key('S') || key(VK_RIGHT) || key('D');
        }
        // Any key or mouse button counts as using the keyboard.
        bool any_key = false;
        if (GetForegroundWindow() == s.parent) {
            for (int code = 1; code < 0xff && !any_key; code++) {
                any_key = (GetAsyncKeyState(code) & 0x8000) != 0;
            }
        }
        Device new_device = device;
        if (any_key && !any_key_held) {
            new_device = Device::Keyboard;
        }
        any_key_held = any_key;

        int pad_count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&pad_count);
        bool any_pad = false;
        for (int i = 0; ids && i < pad_count; i++) {
            SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]);
            if (!pad) {
                pad = SDL_OpenGamepad(ids[i]);
                if (!pad) {
                    continue;
                }
                pads.opened.push_back(pad);
            }
            if (first_pass) {
                // A controller that is plugged in is taken as the device in use until a key
                // says otherwise.
                new_device = device_of(pad);
            }
            const auto button = [&](SDL_GamepadButton which) {
                return SDL_GetGamepadButton(pad, which);
            };
            const int stick = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
            const bool confirm = button(SDL_GAMEPAD_BUTTON_SOUTH);
            const bool cancel = button(SDL_GAMEPAD_BUTTON_EAST);
            const int stick_y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
            const bool left = button(SDL_GAMEPAD_BUTTON_DPAD_UP) ||
                              button(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || stick < -16000 ||
                              stick_y < -16000;
            const bool right = button(SDL_GAMEPAD_BUTTON_DPAD_DOWN) ||
                               button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || stick > 16000 ||
                               stick_y > 16000;
            bool used = confirm || cancel || left || right;
            for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && !used; b++) {
                used = button(static_cast<SDL_GamepadButton>(b));
            }
            down[Confirm] |= confirm;
            down[Cancel] |= cancel;
            down[Left] |= left;
            down[Right] |= right;
            if (used) {
                any_pad = true;
                if (!any_pad_held) {
                    new_device = device_of(pad);
                }
            }
        }
        SDL_free(ids);
        any_pad_held = any_pad;
        first_pass = false;
        if (new_device != device) {
            device = new_device;
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
        if (pressed(Cancel) && selected != 1) {
            // Back steps onto Skip; only the accept button chooses.
            selected = 1;
            dirty = true;
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
            // The two answers as plain lines, the chosen one bright with the marker beside it,
            // as in the game's own menus.
            const std::wstring labels[2] = {L"Prepare", L"Skip"};
            const int line_size = s.Px(44);
            for (int i = 0; i < 2; i++) {
                const int w = s.TextWidth(labels[i], line_size);
                const int x = (s.width - w) / 2;
                const bool chosen = selected == i;
                s.Text(labels[i], x, y, line_size, chosen ? Ink : DimInk);
                if (chosen) {
                    const int m = s.Px(13);
                    const int cx = x - s.Px(30), cy = y + line_size * 58 / 100;
                    const POINT marker[3] = {{cx - m / 2, cy - m}, {cx - m / 2, cy + m}, {cx + m, cy}};
                    const HBRUSH brush = CreateSolidBrush(Ink);
                    const HGDIOBJ old_brush = SelectObject(s.mem, brush);
                    const HGDIOBJ old_pen = SelectObject(s.mem, GetStockObject(NULL_PEN));
                    Polygon(s.mem, marker, 3);
                    SelectObject(s.mem, old_pen);
                    SelectObject(s.mem, old_brush);
                    DeleteObject(brush);
                }
                areas[i] = {x - s.Px(50), y - s.Px(8), x + w + s.Px(50), y + line_size + s.Px(14)};
                y += s.Px(72);
            }
            // One hint, bottom right, with the accept button of the device in use.
            using Button = Impl::Button;
            const Button button = device == Device::Keyboard ? Button::Key
                                  : device == Device::Xbox   ? Button::Round
                                                             : Button::Cross;
            const std::wstring key = device == Device::Xbox ? L"A" : L"Enter";
            const RECT cap = s.Option(0, 0, button, key, L"", false, false);
            const int cap_w = cap.right - cap.left - s.Px(28) - s.Px(16);
            const int hint_size = s.Px(26);
            const int right = s.width - s.Px(64), bottom = s.height - s.Px(64);
            s.Option(right - cap_w, bottom - s.Px(46), button, key, L"", true, true);
            const std::wstring hint = L"ACCEPT";
            s.Text(hint, right - cap_w - s.Px(18) - s.TextWidth(hint, hint_size),
                   bottom - s.Px(46) + (s.Px(46) - hint_size) / 2 - s.Px(2), hint_size, DimInk);
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
