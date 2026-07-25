#pragma once

#include <string>

namespace pcbview::input {

// One frame of controller state, already dead-zoned and normalised. Sticks and
// gyro are CONTINUOUS (apply them scaled by the frame's dt); the `pressed*`
// flags are EDGE-triggered and true only on the poll where the button went
// down, so they can drive one-shot actions like a view preset.
struct GamepadState {
    bool connected = false;
    std::string name;

    // [-1, 1]. Y is already flipped to "up is positive" -- SDL reports
    // sticks with +Y downward, which would invert pitch against the mouse.
    float leftX = 0.0f, leftY = 0.0f;
    float rightX = 0.0f, rightY = 0.0f;
    // [0, 1].
    float leftTrigger = 0.0f, rightTrigger = 0.0f;

    // Face buttons, by POSITION not label: south/east/west/north is Sony's
    // cross/circle/square/triangle and Xbox's A/B/X/Y. SDL normalises the
    // physical layout, so one mapping serves both pads.
    bool pressedSouth = false, pressedEast = false;
    bool pressedWest = false, pressedNorth = false;
    bool pressedStart = false, pressedBack = false;
    bool pressedLeftStick = false, pressedRightStick = false;
    bool pressedDpadUp = false, pressedDpadDown = false;
    bool pressedDpadLeft = false, pressedDpadRight = false;
    bool pressedLeftShoulder = false, pressedRightShoulder = false;

    bool heldLeftShoulder = false, heldRightShoulder = false;

    // Angular velocity in rad/s, present on pads with a gyro (DualSense).
    // Zero and hasGyro=false on an Xbox pad, which has none.
    bool hasGyro = false;
    float gyroX = 0.0f, gyroY = 0.0f, gyroZ = 0.0f;

    // True when any stick or trigger is off-centre, i.e. the user is actively
    // driving the view. Drives the fast-movement raster downgrade, the same as
    // holding a mouse button.
    bool steering = false;
};

// Owns SDL's gamepad subsystem and the first connected controller. Hot-plug is
// handled inside poll(): unplugging and replugging mid-session just works.
//
// SDL is initialised for INPUT ONLY -- no video, no audio, no event loop of its
// own. It is deliberately kept behind this header (the SDL types are hidden
// behind void*) so SDL's headers never reach the Qt/render translation units.
class Gamepad {
public:
    Gamepad();
    ~Gamepad();
    Gamepad(const Gamepad&) = delete;
    Gamepad& operator=(const Gamepad&) = delete;

    // False when SDL's gamepad subsystem could not start at all. The app must
    // stay fully usable in that case -- a controller is an addition, never a
    // requirement.
    bool available() const { return sdlReady_; }

    // Pump SDL, adopt or drop controllers, and return this frame's state.
    const GamepadState& poll();
    const GamepadState& state() const { return state_; }

private:
    void adoptFirstConnected();

    bool sdlReady_ = false;
    void* pad_ = nullptr;  // SDL_Gamepad*
    GamepadState state_;
    unsigned prevButtons_ = 0;
};

}  // namespace pcbview::input
