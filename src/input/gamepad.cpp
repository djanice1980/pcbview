#include "input/gamepad.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>

namespace pcbview::input {
namespace {

// Radial dead zone. Applied to the stick as a VECTOR, not per-axis: a per-axis
// dead zone leaves a cross-shaped live region, so a diagonal push registers
// before a straight one and slow diagonal moves snap to an axis.
constexpr float kStickDeadZone = 0.16f;
// Triggers rest at zero but chatter a little; ignore the bottom of the range.
constexpr float kTriggerFloor = 0.06f;

float axisNorm(int16_t v) {
    // SDL's range is [-32768, 32767]; the asymmetry means a full negative push
    // would exceed -1 if divided by 32767.
    return v < 0 ? static_cast<float>(v) / 32768.0f
                 : static_cast<float>(v) / 32767.0f;
}

// Rescale the live region so it starts at zero just outside the dead zone,
// instead of jumping straight to kStickDeadZone worth of speed.
void applyDeadZone(float& x, float& y) {
    const float mag = std::sqrt(x * x + y * y);
    if (mag < kStickDeadZone) {
        x = y = 0.0f;
        return;
    }
    const float scaled = (mag - kStickDeadZone) / (1.0f - kStickDeadZone);
    const float k = (scaled > 1.0f ? 1.0f : scaled) / mag;
    x *= k;
    y *= k;
}

}  // namespace

Gamepad::Gamepad() {
    // Input only. SDL must not touch video: Qt owns the window and the event
    // loop, and asking SDL for a video subsystem here would have it create its
    // own hidden window and message pump.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    // Report a DualSense as a DualSense (gyro, touchpad, proper mapping)
    // rather than letting it fall back to a generic HID joystick.
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "gamepad: SDL_Init failed (%s); controller "
                             "support disabled\n",
                     SDL_GetError());
        return;
    }
    sdlReady_ = true;
    adoptFirstConnected();
    if (!pad_) {
        // Distinguish "nothing plugged in" from "seen, but SDL has no gamepad
        // MAPPING for it" -- the latter shows up as a joystick with zero
        // gamepads, and means the device needs a mapping entry rather than a
        // cable. Logged once; hot-plug is still handled from poll().
        int joys = 0;
        if (SDL_JoystickID* ids = SDL_GetJoysticks(&joys)) SDL_free(ids);
        std::printf("gamepad: none detected at startup (%d joystick(s) "
                    "visible to SDL); hot-plug is supported\n",
                    joys);
        std::fflush(stdout);
    }
}

Gamepad::~Gamepad() {
    if (pad_) SDL_CloseGamepad(static_cast<SDL_Gamepad*>(pad_));
    if (sdlReady_) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

void Gamepad::adoptFirstConnected() {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (!ids) return;
    if (count > 0) {
        SDL_Gamepad* pad = SDL_OpenGamepad(ids[0]);
        if (pad) {
            pad_ = pad;
            const char* n = SDL_GetGamepadName(pad);
            state_.name = n ? n : "controller";
            // Gyro is optional and only some pads have it; enabling it on a
            // pad without one simply fails, which is not an error.
            state_.hasGyro =
                SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO) &&
                SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true);
            std::printf("gamepad: %s%s\n", state_.name.c_str(),
                        state_.hasGyro ? " (gyro)" : "");
            std::fflush(stdout);
        }
    }
    SDL_free(ids);
}

const GamepadState& Gamepad::poll() {
    if (!sdlReady_) return state_;

    // Drain SDL's queue. Without this, hot-plug is never noticed and the
    // internal state never advances.
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_GAMEPAD_REMOVED && pad_ &&
            e.gdevice.which ==
                SDL_GetGamepadID(static_cast<SDL_Gamepad*>(pad_))) {
            SDL_CloseGamepad(static_cast<SDL_Gamepad*>(pad_));
            pad_ = nullptr;
            state_ = GamepadState{};
            prevButtons_ = 0;
        }
    }
    if (!pad_) adoptFirstConnected();

    auto* pad = static_cast<SDL_Gamepad*>(pad_);
    if (!pad) {
        state_.connected = false;
        state_.steering = false;
        return state_;
    }
    state_.connected = true;

    state_.leftX = axisNorm(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX));
    state_.leftY = -axisNorm(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY));
    state_.rightX = axisNorm(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX));
    state_.rightY = -axisNorm(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY));
    applyDeadZone(state_.leftX, state_.leftY);
    applyDeadZone(state_.rightX, state_.rightY);

    // Triggers are one-sided: SDL reports 0..32767.
    const float lt = static_cast<float>(SDL_GetGamepadAxis(
                         pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)) /
                     32767.0f;
    const float rt = static_cast<float>(SDL_GetGamepadAxis(
                         pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) /
                     32767.0f;
    state_.leftTrigger = lt < kTriggerFloor ? 0.0f : lt;
    state_.rightTrigger = rt < kTriggerFloor ? 0.0f : rt;

    // Pack the buttons we care about into a bitfield so the edge detection is
    // one xor rather than a dozen remembered bools.
    const SDL_GamepadButton kButtons[] = {
        SDL_GAMEPAD_BUTTON_SOUTH,          SDL_GAMEPAD_BUTTON_EAST,
        SDL_GAMEPAD_BUTTON_WEST,           SDL_GAMEPAD_BUTTON_NORTH,
        SDL_GAMEPAD_BUTTON_START,          SDL_GAMEPAD_BUTTON_BACK,
        SDL_GAMEPAD_BUTTON_LEFT_STICK,     SDL_GAMEPAD_BUTTON_RIGHT_STICK,
        SDL_GAMEPAD_BUTTON_DPAD_UP,        SDL_GAMEPAD_BUTTON_DPAD_DOWN,
        SDL_GAMEPAD_BUTTON_DPAD_LEFT,      SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
        SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
    };
    unsigned now = 0;
    for (unsigned i = 0; i < sizeof(kButtons) / sizeof(kButtons[0]); ++i)
        if (SDL_GetGamepadButton(pad, kButtons[i])) now |= 1u << i;
    const unsigned went = now & ~prevButtons_;  // newly pressed this poll
    prevButtons_ = now;

    state_.pressedSouth = (went & (1u << 0)) != 0;
    state_.pressedEast = (went & (1u << 1)) != 0;
    state_.pressedWest = (went & (1u << 2)) != 0;
    state_.pressedNorth = (went & (1u << 3)) != 0;
    state_.pressedStart = (went & (1u << 4)) != 0;
    state_.pressedBack = (went & (1u << 5)) != 0;
    state_.pressedLeftStick = (went & (1u << 6)) != 0;
    state_.pressedRightStick = (went & (1u << 7)) != 0;
    state_.pressedDpadUp = (went & (1u << 8)) != 0;
    state_.pressedDpadDown = (went & (1u << 9)) != 0;
    state_.pressedDpadLeft = (went & (1u << 10)) != 0;
    state_.pressedDpadRight = (went & (1u << 11)) != 0;
    state_.pressedLeftShoulder = (went & (1u << 12)) != 0;
    state_.pressedRightShoulder = (went & (1u << 13)) != 0;
    state_.heldLeftShoulder = (now & (1u << 12)) != 0;
    state_.heldRightShoulder = (now & (1u << 13)) != 0;

    if (state_.hasGyro) {
        float g[3] = {0.0f, 0.0f, 0.0f};
        if (SDL_GetGamepadSensorData(pad, SDL_SENSOR_GYRO, g, 3)) {
            state_.gyroX = g[0];
            state_.gyroY = g[1];
            state_.gyroZ = g[2];
        }
    }

    state_.steering = state_.leftX != 0.0f || state_.leftY != 0.0f ||
                      state_.rightX != 0.0f || state_.rightY != 0.0f ||
                      state_.leftTrigger > 0.0f || state_.rightTrigger > 0.0f;
    return state_;
}

}  // namespace pcbview::input
