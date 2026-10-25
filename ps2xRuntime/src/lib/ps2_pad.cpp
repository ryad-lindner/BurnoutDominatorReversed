#include "runtime/ps2_pad.h"
#include "ps2_host_backend.h"
#include "runtime/gamepad_rumble.h"
#include "runtime/host_settings.h"
#include "runtime/settings_menu.h"
#include <algorithm>
#include <mutex>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

#if defined(PS2X_HAS_SDL2)
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <iostream>
#endif

namespace
{
    constexpr uint8_t kPadAnalogMarker = 0x73;
    constexpr uint8_t kPadStickCenter = 0x80;

    constexpr uint16_t PAD_LEFT = 0x0080u;
    constexpr uint16_t PAD_DOWN = 0x0040u;
    constexpr uint16_t PAD_RIGHT = 0x0020u;
    constexpr uint16_t PAD_UP = 0x0010u;
    constexpr uint16_t PAD_START = 0x0008u;
    constexpr uint16_t PAD_R3 = 0x0004u;
    constexpr uint16_t PAD_L3 = 0x0002u;
    constexpr uint16_t PAD_SELECT = 0x0001u;
    constexpr uint16_t PAD_SQUARE = 0x8000u;
    constexpr uint16_t PAD_CROSS = 0x4000u;
    constexpr uint16_t PAD_CIRCLE = 0x2000u;
    constexpr uint16_t PAD_TRIANGLE = 0x1000u;
    constexpr uint16_t PAD_R1 = 0x0800u;
    constexpr uint16_t PAD_L1 = 0x0400u;
    constexpr uint16_t PAD_R2 = 0x0200u;
    constexpr uint16_t PAD_L2 = 0x0100u;

    // Sticks in -1..1 and the D-pad, face, shoulder and stick buttons as PAD_* bits.
    struct GamepadInput
    {
        uint16_t pressed = 0u;
        float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;
    };

#if defined(PS2X_HAS_SDL2)
    // Every attached game controller is open. The player's, g_controller, is the first to press
    // a button and stays theirs until it is disconnected; then the next press takes the pad, as
    // on consoles. Only the pad thread changes them; g_rumbleMutex keeps g_controller open while
    // the game thread sets its motors.
    struct OpenController
    {
        SDL_GameController *controller;
        uint16_t pressed;
        // Until 250 ms after the open, buttons down are taken as the resting state, not as a
        // press: a pedal at rest may be one, and some drivers report the state only later.
        Uint32 opened;
    };
    std::vector<OpenController> g_controllers;
    // Controllers whose open failed, retried a while later: udev may not have applied the
    // device's permissions yet.
    std::vector<std::pair<SDL_JoystickID, Uint32>> g_failedOpens;
    SDL_GameController *g_controller = nullptr;
    std::mutex g_rumbleMutex;
    uint32_t g_rumble = 0u; // low frequency motor << 16 | high frequency motor
    bool g_vibration = false;

    // The game sets the motors when they change; a long duration keeps them running until then.
    void rumble(SDL_GameController *controller, uint32_t motors)
    {
        SDL_GameControllerRumble(controller, static_cast<Uint16>(motors >> 16), static_cast<Uint16>(motors),
                                 motors ? 10000u : 0u);
    }

    uint16_t pressedButtons(SDL_GameController *controller)
    {
        static constexpr std::pair<SDL_GameControllerButton, uint16_t> kButtons[] = {
            {SDL_CONTROLLER_BUTTON_DPAD_UP, PAD_UP}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, PAD_DOWN},
            {SDL_CONTROLLER_BUTTON_DPAD_LEFT, PAD_LEFT}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, PAD_RIGHT},
            {SDL_CONTROLLER_BUTTON_A, PAD_CROSS}, {SDL_CONTROLLER_BUTTON_B, PAD_CIRCLE},
            {SDL_CONTROLLER_BUTTON_X, PAD_SQUARE}, {SDL_CONTROLLER_BUTTON_Y, PAD_TRIANGLE},
            {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, PAD_L1}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, PAD_R1},
            {SDL_CONTROLLER_BUTTON_START, PAD_START}, {SDL_CONTROLLER_BUTTON_BACK, PAD_SELECT},
            {SDL_CONTROLLER_BUTTON_LEFTSTICK, PAD_L3}, {SDL_CONTROLLER_BUTTON_RIGHTSTICK, PAD_R3}};
        uint16_t pressed = 0u;
        for (const auto &[button, mask] : kButtons)
            if (SDL_GameControllerGetButton(controller, button))
                pressed |= mask;
        constexpr Sint16 kTriggerPressed = 8000;
        if (SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > kTriggerPressed)
            pressed |= PAD_L2;
        if (SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > kTriggerPressed)
            pressed |= PAD_R2;
        return pressed;
    }

    bool isKnown(SDL_JoystickID id)
    {
        return std::any_of(g_controllers.begin(), g_controllers.end(), [id](const OpenController &open)
                           { return SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(open.controller)) == id; }) ||
               std::any_of(g_failedOpens.begin(), g_failedOpens.end(), [id](const auto &failed)
                           { return failed.first == id; });
    }

    // SDL2 game controllers: Xbox, PlayStation 3/4/5, Switch and the SDL_GameControllerDB
    // pads, over USB or Bluetooth, with or without Steam. Opened and pumped by the thread
    // that reads the pad.
    bool readGamepad(GamepadInput &input)
    {
        static const bool ready = []
        {
            SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
            // Without SDL video nothing pumps window messages on this thread: SDL handles
            // device and raw input messages on its own thread, and Xbox pads are polled
            // through XInput instead of raw input.
            SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
            SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
            // SDL's handlers would turn SIGINT and SIGTERM into an SDL_QUIT nobody reads.
            SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
            SDL_SetMainReady();
            if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0)
            {
                std::cerr << "[pad] SDL game controllers unavailable: " << SDL_GetError() << std::endl;
                return false;
            }
            if (!ps2x::hostSettings().gamepadMappings.empty())
                SDL_GameControllerAddMappingsFromFile(ps2x::hostSettings().gamepadMappings.c_str());
            return true;
        }();
        if (!ready)
            return false;
        // SDL 3 (sdl2-compat on Arch and Fedora) reads udev's hotplug events only in the event
        // loop. Nothing reads the queue: the window is GLFW's.
        SDL_PumpEvents();
        SDL_GameControllerUpdate();
        SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
        for (size_t i = g_controllers.size(); i-- > 0;)
        {
            SDL_GameController *controller = g_controllers[i].controller;
            if (SDL_GameControllerGetAttached(controller))
                continue;
            std::cerr << "[pad] gamepad disconnected" << std::endl;
            if (controller == g_controller)
            {
                std::lock_guard lock(g_rumbleMutex);
                g_controller = nullptr;
            }
            SDL_GameControllerClose(controller);
            g_controllers.erase(g_controllers.begin() + static_cast<std::ptrdiff_t>(i));
        }
        const Uint32 now = SDL_GetTicks();
        std::erase_if(g_failedOpens, [now](const auto &failed)
                      { return now - failed.second >= 2000u; });
        for (int index = 0; index < SDL_NumJoysticks(); ++index)
        {
            const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(index);
            if (!SDL_IsGameController(index) || isKnown(id))
                continue;
            if (SDL_GameController *controller = SDL_GameControllerOpen(index))
            {
                g_controllers.push_back({controller, 0u, now});
                std::cerr << "[pad] gamepad connected: " << SDL_GameControllerName(controller) << std::endl;
            }
            else
            {
                std::cerr << "[pad] gamepad not opened: " << SDL_GetError() << std::endl;
                g_failedOpens.emplace_back(id, now);
            }
        }
        for (OpenController &open : g_controllers)
        {
            const uint16_t pressed = pressedButtons(open.controller);
            if (now - open.opened >= 250u && !g_controller && (pressed & ~open.pressed) != 0u)
            {
                std::lock_guard lock(g_rumbleMutex);
                g_controller = open.controller;
                if (g_vibration)
                    rumble(g_controller, g_rumble);
                std::cerr << "[pad] gamepad: " << SDL_GameControllerName(g_controller) << std::endl;
            }
            open.pressed = pressed;
            if (open.controller == g_controller)
                input.pressed = pressed;
        }
        SDL_GameController *controller = g_controller;
        if (!controller)
            return false;

        if (ps2x::hostSettings().vibration != g_vibration)
        {
            std::lock_guard lock(g_rumbleMutex);
            g_vibration = !g_vibration;
            rumble(controller, g_vibration ? g_rumble : 0u);
        }
        auto axis = [&](SDL_GameControllerAxis which)
        { return std::max(-1.0f, SDL_GameControllerGetAxis(controller, which) / 32767.0f); };
        input.lx = axis(SDL_CONTROLLER_AXIS_LEFTX);
        input.ly = axis(SDL_CONTROLLER_AXIS_LEFTY);
        input.rx = axis(SDL_CONTROLLER_AXIS_RIGHTX);
        input.ry = axis(SDL_CONTROLLER_AXIS_RIGHTY);
        static bool reported = false;
        if (!reported && input.pressed != 0u)
        {
            reported = true;
            std::cerr << "[pad] gamepad buttons received" << std::endl;
        }
        return true;
    }
#else
    // Without SDL2: raylib's gamepads (GLFW and its built-in SDL_GameControllerDB subset).
    bool readGamepad(GamepadInput &input)
    {
        int gamepad = -1;
        for (int index = 0; index < 4 && gamepad < 0; ++index)
            if (IsGamepadAvailable(index))
                gamepad = index;
        if (gamepad < 0)
            return false;
        static constexpr std::pair<int, uint16_t> kButtons[] = {
            {GAMEPAD_BUTTON_LEFT_FACE_UP, PAD_UP}, {GAMEPAD_BUTTON_LEFT_FACE_DOWN, PAD_DOWN},
            {GAMEPAD_BUTTON_LEFT_FACE_LEFT, PAD_LEFT}, {GAMEPAD_BUTTON_LEFT_FACE_RIGHT, PAD_RIGHT},
            {GAMEPAD_BUTTON_RIGHT_FACE_DOWN, PAD_CROSS}, {GAMEPAD_BUTTON_RIGHT_FACE_RIGHT, PAD_CIRCLE},
            {GAMEPAD_BUTTON_RIGHT_FACE_LEFT, PAD_SQUARE}, {GAMEPAD_BUTTON_RIGHT_FACE_UP, PAD_TRIANGLE},
            {GAMEPAD_BUTTON_LEFT_TRIGGER_1, PAD_L1}, {GAMEPAD_BUTTON_RIGHT_TRIGGER_1, PAD_R1},
            {GAMEPAD_BUTTON_LEFT_TRIGGER_2, PAD_L2}, {GAMEPAD_BUTTON_RIGHT_TRIGGER_2, PAD_R2},
            {GAMEPAD_BUTTON_MIDDLE_RIGHT, PAD_START}, {GAMEPAD_BUTTON_MIDDLE_LEFT, PAD_SELECT},
            {GAMEPAD_BUTTON_LEFT_THUMB, PAD_L3}, {GAMEPAD_BUTTON_RIGHT_THUMB, PAD_R3}};
        for (const auto &[button, mask] : kButtons)
            if (IsGamepadButtonDown(gamepad, button))
                input.pressed |= mask;
        input.lx = GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_LEFT_X);
        input.ly = GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_LEFT_Y);
        input.rx = GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_RIGHT_X);
        input.ry = GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_RIGHT_Y);
        return true;
    }
#endif
}

bool PSPadBackend::readState(int /*port*/, int /*slot*/, uint8_t *data, size_t size)
{
    if (!data || size < 32)
        return false;

    std::memset(data, 0, 32);
    data[0] = 0x01;
    data[1] = kPadAnalogMarker;
    data[2] = 0xFF;
    data[3] = 0xFF;
    data[4] = data[5] = data[6] = data[7] = kPadStickCenter;

    uint16_t btns = 0xFFFFu;
    auto clearBit = [&btns](uint16_t mask)
    { btns &= ~mask; };

    // The player's gamepad and the keyboard both drive the DualShock 2, at any time.
    GamepadInput pad;
    if (readGamepad(pad))
    {
        clearBit(pad.pressed);

        // Radial dead zone, then the remaining range spread over 0..255.
        const float deadzone = ps2x::hostSettings().stickDeadzone;
        auto stick = [&](float x, float y, uint8_t &outX, uint8_t &outY)
        {
            const float length = std::sqrt(x * x + y * y);
            const float scale = length <= deadzone ? 0.0f
                                                   : std::min(1.0f, (length - deadzone) / (1.0f - deadzone)) / length;
            auto toByte = [](float v)
            { return static_cast<uint8_t>(std::clamp(std::lround(128.0f + v * 127.5f), 0l, 255l)); };
            outX = toByte(x * scale);
            outY = toByte(y * scale);
        };
        stick(pad.lx, pad.ly, data[6], data[7]);
        stick(pad.rx, pad.ry, data[4], data[5]);
    }

    // Keyboard and mouse, as bound in settings.ini (padButtonNames() order); not while the
    // settings menu has them.
    if (!ps2x::settingsMenuCapturesInput())
    {
        static constexpr uint16_t kMasks[16] = {PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT, PAD_CROSS, PAD_CIRCLE,
                                                PAD_SQUARE, PAD_TRIANGLE, PAD_L1, PAD_R1, PAD_L2, PAD_R2,
                                                PAD_START, PAD_SELECT, PAD_L3, PAD_R3};
        const bool alt = IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);
        // A wheel notch holds its binding for three polls, then leaves three released.
        static int wheelHeld = 0, wheelGap = 0, wheelDirection = 0;
        if (wheelHeld > 0)
            --wheelHeld;
        else if (wheelGap > 0)
            --wheelGap;
        else if ((wheelDirection = ps2x::takeMouseWheelNotch()) != 0)
        {
            wheelHeld = 2;
            wheelGap = 3;
        }
        const int wheelInput = wheelHeld > 0 || (wheelGap == 3 && wheelDirection != 0)
                                   ? (wheelDirection > 0 ? ps2x::kMouseWheelUp : ps2x::kMouseWheelDown)
                                   : 0;
        const auto &keys = ps2x::hostSettings().keys;
        for (size_t button = 0; button < keys.size(); ++button)
            for (const int input : keys[button])
            {
                const bool down = input == ps2x::kMouseWheelUp || input == ps2x::kMouseWheelDown
                                      ? input == wheelInput
                                  : input >= ps2x::kMouseInput ? IsMouseButtonDown(input - ps2x::kMouseInput)
                                  : input == KEY_ENTER       ? IsKeyDown(input) && !alt // Alt+Enter: full screen
                                  : input == KEY_ESCAPE      ? IsKeyDown(input) && !ps2x::escapeAsksToQuit()
                                  : input > 0                ? IsKeyDown(input)
                                                             : false;
                if (down)
                    clearBit(kMasks[button]);
            }
    }

    data[2] = static_cast<uint8_t>(btns & 0xFF);
    data[3] = static_cast<uint8_t>(btns >> 8);
    return true;
}

void ps2x::setGamepadRumble(bool small, uint8_t large)
{
#if defined(PS2X_HAS_SDL2)
    std::lock_guard lock(g_rumbleMutex);
    g_rumble = large * 257u << 16 | (small ? 0xFFFFu : 0u);
    if (g_controller && g_vibration)
        rumble(g_controller, g_rumble);
#else
    (void)small;
    (void)large;
#endif
}
