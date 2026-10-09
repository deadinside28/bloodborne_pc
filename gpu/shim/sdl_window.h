// bbport: the game window. Created by the VideoOut driver on first open; the
// event pump runs on the port's window thread (see window.cpp).
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include "common/types.h"

struct SDL_Window;

namespace Frontend {

enum class WindowSystemType : u8 { Headless, Windows, X11, Wayland, Metal };

struct WindowSystemInfo {
    void* display_connection = nullptr;
    void* render_surface = nullptr;
    float render_surface_scale = 1.0f;
    WindowSystemType type = WindowSystemType::Headless;
};

class WindowSDL {
public:
    WindowSDL(s32 width, s32 height, const char* title);
    ~WindowSDL();
    s32 GetWidth() const { return width.load(std::memory_order_relaxed); }
    s32 GetHeight() const { return height.load(std::memory_order_relaxed); }
    SDL_Window* GetSDLWindow() const { return window; }
    WindowSystemInfo GetWindowInfo() const { return window_info; }
    bool IsOpen() const { return is_open.load(std::memory_order_relaxed); }
    /// Processes pending window events. Returns false once the user closed the window.
    bool PollEvents();
    /// Keyboard text entry for the system IME dialog; typed text shows in the title bar.
    void BeginTextInput(const std::string& initial, const std::string& prompt);
    /// 0 while typing, 1 confirmed (Enter), 2 cancelled (Escape); text is UTF-8.
    int PollTextInput(std::string& text);
    /// Step 0 telemetry: accumulated SDL mouse motion since the last call (xrel/yrel
    /// pixels). Consumes and resets to zero; thread-safe for the pad thread.
    void ConsumeMouseDelta(double& dx, double& dy);
    /// Step 1 mouse look: relative-mode capture (F1 / middle-click toggle).
    /// Thread-safe: set on the window thread, read on the pad thread.
    void SetMouseCaptured(bool enabled);
    bool IsMouseCaptured() const { return mouse_captured.load(std::memory_order_relaxed); }

private:
    std::atomic<s32> width, height;
    std::atomic<bool> is_open{true};
    std::mutex text_mutex;
    bool text_requested{}, text_active{};
    int text_state{};
    std::string text, text_prompt, base_title;
    void UpdateTextTitle();
    void UpdateCursor();
    u64 last_mouse_motion_ms{}; ///< SDL_GetTicks of the last mouse motion (UpdateCursor)
    bool cursor_hidden{};
    std::mutex mouse_mutex;
    double mouse_dx{}, mouse_dy{}; ///< accumulated xrel/yrel since ConsumeMouseDelta
    std::atomic<bool> mouse_captured{false}; ///< relative mode for mouse look (F1 toggle)
    SDL_Window* window{};
    WindowSystemInfo window_info{};
};

} // namespace Frontend
