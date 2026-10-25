#include "runtime/host_window.h"

#include "runtime/host_settings.h"

#include "raylib.h"
#include "rlgl.h"

#if defined(__linux__) && !defined(__ANDROID__) && !defined(PLATFORM_VITA)
#define PS2X_GLFW_LINUX 1
#include <GLFW/glfw3.h>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#endif

#ifdef PS2X_GLFW_LINUX
// raylib sets its window hints, then creates the window inside InitWindow, and glfwDefaultWindowHints
// clears every hint set before: the Wayland app id and the X11 class (what a desktop entry's StartupWMClass is matched
// with, like X11's RESOURCE_NAME) can only be given in between. The link wraps glfwCreateWindow
// (-Wl,--wrap in ps2xRuntime/CMakeLists.txt) to do that.
extern "C"
{
    GLFWwindow *__real_glfwCreateWindow(int width, int height, const char *title, GLFWmonitor *monitor,
                                        GLFWwindow *share);

    GLFWwindow *__wrap_glfwCreateWindow(int width, int height, const char *title, GLFWmonitor *monitor,
                                        GLFWwindow *share)
    {
        const char *name = std::getenv("RESOURCE_NAME");
        std::string appId = name ? name : "";
        if (appId.empty())
        {
            std::error_code error;
            appId = std::filesystem::read_symlink("/proc/self/exe", error).filename().string();
        }
        if (!appId.empty())
        {
            glfwWindowHintString(GLFW_WAYLAND_APP_ID, appId.c_str());
            glfwWindowHintString(GLFW_X11_CLASS_NAME, appId.c_str());
            glfwWindowHintString(GLFW_X11_INSTANCE_NAME, appId.c_str());
        }
        return __real_glfwCreateWindow(width, height, title, monitor, share);
    }
}
#endif

namespace ps2x
{
    DisplayServer pickDisplayServer(std::string_view setting, bool steam, bool x11, bool wayland)
    {
        if (setting == "wayland" && wayland)
            return DisplayServer::Wayland;
        if (setting == "x11" && x11)
            return DisplayServer::X11;
        // Steam Input applies a game's controller profile while the game's window has the focus,
        // which Steam follows for X11 windows only: after an Alt+Tab a native Wayland window got
        // the desktop profile back (a Steam Controller typed keys). Started from Steam, auto
        // prefers X11 (XWayland).
        if (steam && x11)
            return DisplayServer::X11;
        if (wayland)
            return DisplayServer::Wayland;
        return x11 ? DisplayServer::X11 : DisplayServer::Any;
    }

#ifdef PS2X_GLFW_LINUX
    namespace
    {
        struct Windowed
        {
            int x = 0, y = 0, width = 0, height = 0;
        };
        Windowed g_windowed;

        bool onWayland()
        {
            return glfwGetPlatform() == GLFW_PLATFORM_WAYLAND;
        }

        GLFWmonitor *currentMonitor()
        {
            int count = 0;
            GLFWmonitor **monitors = glfwGetMonitors(&count);
            // A Wayland client does not know where its window is: GLFW has no window position there.
            const int index = onWayland() ? 0 : GetCurrentMonitor();
            return index >= 0 && index < count ? monitors[index] : glfwGetPrimaryMonitor();
        }

        // raylib goes on after a failed InitWindow and crashes in its first GL call, and GLFW does not
        // fall back to X11 by itself once a Wayland session selects Wayland: a hidden window with
        // raylib's OpenGL 3.3 context shows whether the Wayland connection and its EGL driver work
        // (EGL fails on NVIDIA without egl-wayland, for one).
        bool waylandWindowOpens()
        {
            glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_WAYLAND);
            bool opened = false;
            if (glfwInit())
            {
                glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
                glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
                glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
                glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
                if (GLFWwindow *probe = __real_glfwCreateWindow(64, 64, "", nullptr, nullptr))
                {
                    glfwDestroyWindow(probe);
                    opened = true;
                }
            }
            const char *error = nullptr;
            if (!opened && glfwGetError(&error) != GLFW_NO_ERROR && error)
                std::cerr << "[display] no Wayland window: " << error << std::endl;
            glfwTerminate();
            return opened;
        }
    }

    bool chooseDisplayServer()
    {
        const std::string &wanted = hostSettings().displayServer;
        const bool x11Built = glfwPlatformSupported(GLFW_PLATFORM_X11);
        const bool waylandBuilt = glfwPlatformSupported(GLFW_PLATFORM_WAYLAND);
        const bool x11 = x11Built && std::getenv("DISPLAY");
        const bool wayland = waylandBuilt && std::getenv("WAYLAND_DISPLAY");
        const char *game = std::getenv("SteamGameId");
        const bool steam = game && *game;
        DisplayServer choice = pickDisplayServer(wanted, steam, x11, wayland);

        if ((wanted == "wayland" && !wayland) || (wanted == "x11" && !x11))
        {
            const bool built = wanted == "wayland" ? waylandBuilt : x11Built;
            std::cerr << "[display] " << (wanted == "wayland" ? "Wayland" : "X11")
                      << (built ? ": no such display in this session" : " is not built into this GLFW")
                      << ", choosing automatically" << std::endl;
        }
        else if (wanted == "auto" && steam && choice == DisplayServer::X11)
            std::cout << "[display] started from Steam: X11, for Steam Input (display_server = wayland forces Wayland)"
                      << std::endl;

        if (choice == DisplayServer::Any)
            return false; // neither display in the session
        if (choice == DisplayServer::Wayland && !waylandWindowOpens())
        {
            if (!x11)
                return false;
            choice = DisplayServer::X11;
            std::cerr << "[display] falling back to X11" << std::endl;
        }
        glfwInitHint(GLFW_PLATFORM, choice == DisplayServer::Wayland ? GLFW_PLATFORM_WAYLAND
                                                                     : GLFW_PLATFORM_X11);
        return true;
    }

    void reportDisplayServer()
    {
        GLFWwindow *window = static_cast<GLFWwindow *>(GetWindowHandle());
        if (!window)
            return;
        const int platform = glfwGetPlatform();
        std::cout << "[display] window system: "
                  << (platform == GLFW_PLATFORM_WAYLAND ? "Wayland" : platform == GLFW_PLATFORM_X11 ? "X11" : "other")
                  << std::endl;
        int width = 0, height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        std::cout << "[display] window " << GetScreenWidth() << "x" << GetScreenHeight() << ", framebuffer " << width
                  << "x" << height << std::endl;
    }

    void fitViewportToFramebuffer()
    {
        // Wayland sizes windows in logical units and gives them a framebuffer scaled by the output's
        // scale. raylib 5.5 (without its high DPI flag, which would also scale mouse and window
        // coordinates) sets the viewport from the window size: the picture would fill the corner of
        // the framebuffer. The projection stays in window units, so drawing in them covers it all.
        if (!onWayland())
            return;
        int width = 0, height = 0;
        glfwGetFramebufferSize(static_cast<GLFWwindow *>(GetWindowHandle()), &width, &height);
        if (width > 0 && height > 0 && (width != GetScreenWidth() || height != GetScreenHeight()))
            rlViewport(0, 0, width, height);
    }

    bool windowFullscreen()
    {
        // GLFW's own record: it holds the monitor from glfwSetWindowMonitor until the next call. Neither
        // backend learns that the compositor or window manager ended full screen by itself, so F11 then
        // takes one press to leave the state and a second one to enter it again.
        GLFWwindow *window = static_cast<GLFWwindow *>(GetWindowHandle());
        return window && glfwGetWindowMonitor(window) != nullptr;
    }

    void toggleWindowFullscreen()
    {
        GLFWwindow *window = static_cast<GLFWwindow *>(GetWindowHandle());
        if (!window)
            return;
        if (!windowFullscreen())
        {
            GLFWmonitor *monitor = currentMonitor();
            const GLFWvidmode *mode = monitor ? glfwGetVideoMode(monitor) : nullptr;
            if (!mode)
                return;
            glfwGetWindowSize(window, &g_windowed.width, &g_windowed.height);
            if (!onWayland())
                glfwGetWindowPos(window, &g_windowed.x, &g_windowed.y);
            // raylib's ToggleFullscreen asks for the window's size, which X11 answers with a mode
            // switch. The monitor's own mode switches nothing: the compositor gets a fullscreen window.
            glfwSetWindowMonitor(window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
        }
        else
        {
            glfwSetWindowMonitor(window, nullptr, g_windowed.x, g_windowed.y, g_windowed.width, g_windowed.height,
                                 GLFW_DONT_CARE);
        }
        // raylib learns the new size from its window size callback, which ignores it while raylib's own
        // fullscreen flag is set: that flag stays off here. Vsync is asked for again after the monitor
        // change, as raylib's ToggleFullscreen does.
        if (hostSettings().vsync)
            glfwSwapInterval(1);
    }
#else
    bool chooseDisplayServer() { return true; }
    void reportDisplayServer() {}
    void fitViewportToFramebuffer() {}

    bool windowFullscreen()
    {
        return IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE);
    }

    void toggleWindowFullscreen()
    {
        ToggleBorderlessWindowed();
    }
#endif
}
