#pragma once

#include <string_view>

namespace ps2x
{
    enum class DisplayServer
    {
        Any, // GLFW's own choice
        Wayland,
        X11,
    };
    // The window system for settings.ini display_server ("auto", "wayland" or "x11"). x11 and wayland:
    // that window system is built into GLFW and the session has its display; steam: Steam started the
    // game. A forced one that is not available counts as auto.
    DisplayServer pickDisplayServer(std::string_view setting, bool steam, bool x11, bool wayland);

    // Linux: applies settings.ini display_server (Wayland or X11) before raylib creates the window,
    // and falls back to X11 when no Wayland window opens; nothing elsewhere. Call before InitWindow.
    // False when neither can open a window: raylib would crash in InitWindow.
    bool chooseDisplayServer();
    // After InitWindow: names the window system in the log.
    void reportDisplayServer();
    // Call right after BeginDrawing: on Wayland, makes the viewport cover the whole framebuffer, which
    // is larger than the window when the output is scaled. Nothing on X11 and elsewhere.
    void fitViewportToFramebuffer();

    // Full screen: on Linux a real fullscreen window at the monitor's current video mode (no mode
    // switch, so the compositor can scan it out directly and drive adaptive sync); elsewhere
    // raylib's borderless window.
    bool windowFullscreen();
    void toggleWindowFullscreen();
}
