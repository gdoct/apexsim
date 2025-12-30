# Input System Specification

## Overview

The Input System handles all player interactions using Unreal Engine 5's Enhanced Input System. It provides flexible, rebindable controls across multiple device types (keyboard, gamepad, steering wheel) with full accessibility support.

---

## Input Architecture

### Enhanced Input System

Unreal Engine 5's Enhanced Input System provides:
*   **Input Actions:** Abstract gameplay actions (e.g., "Throttle", "Steering")
*   **Input Mapping Contexts:** Device-specific bindings (keyboard vs gamepad vs wheel)
*   **Input Modifiers:** Transform raw input (dead-zones, sensitivity, curves)
*   **Input Triggers:** Determine when actions fire (pressed, released, held, etc.)

---

## Input Actions

Defined in `Content/Input/IA_*.uasset`:

### Driving Actions

*   **`IA_Throttle`**
    *   **Type:** Value, Axis1D
    *   **Range:** 0.0 (no throttle) to 1.0 (full throttle)
    *   **Description:** Accelerator pedal input

*   **`IA_Brake`**
    *   **Type:** Value, Axis1D
    *   **Range:** 0.0 (no brake) to 1.0 (full brake)
    *   **Description:** Brake pedal input

*   **`IA_Steering`**
    *   **Type:** Value, Axis1D
    *   **Range:** -1.0 (full left) to 1.0 (full right)
    *   **Description:** Steering wheel input

*   **`IA_ShiftUp`** (Future)
    *   **Type:** Button
    *   **Description:** Manual transmission shift up

*   **`IA_ShiftDown`** (Future)
    *   **Type:** Button
    *   **Description:** Manual transmission shift down

### Camera Actions

*   **`IA_LookBack`**
    *   **Type:** Button
    *   **Description:** Toggle or hold rear-view camera

*   **`IA_ChangeCamera`**
    *   **Type:** Button
    *   **Description:** Cycle through available camera views

### Menu Actions

*   **`IA_PauseMenu`**
    *   **Type:** Button
    *   **Description:** Open/close pause menu or navigate back

---

## Input Mapping Contexts

Defined in `Content/Input/IMC_*.uasset`:

### IMC_Keyboard

**Keyboard-specific mappings with digital-to-analog conversion:**

*   **W → IA_Throttle**
    *   Value: 1.0
    *   Modifiers: Digital to Analog (ramp-up time: 0.1s, ramp-down time: 0.05s)

*   **S → IA_Brake**
    *   Value: 1.0
    *   Modifiers: Digital to Analog (ramp-up time: 0.05s, ramp-down time: 0.02s)

*   **A → IA_Steering**
    *   Value: -1.0
    *   Modifiers: Digital to Analog (ramp-up time: 0.1s, ramp-down time: 0.1s)

*   **D → IA_Steering**
    *   Value: 1.0
    *   Modifiers: Digital to Analog (ramp-up time: 0.1s, ramp-down time: 0.1s)

*   **C → IA_LookBack**
    *   Trigger: Pressed (hold mode)

*   **V → IA_ChangeCamera**
    *   Trigger: Pressed (fires once per press)

*   **ESC → IA_PauseMenu**
    *   Trigger: Pressed

**Notes:**
*   Digital-to-analog conversion provides smooth ramping for keyboard inputs
*   Faster brake ramp than throttle for quick emergency stops
*   Steering ramps prevent instant full-lock inputs

---

### IMC_Gamepad

**Gamepad-specific mappings using analog sticks and triggers:**

*   **Right Trigger (RT) → IA_Throttle**
    *   Raw axis: 0.0 to 1.0
    *   No modifiers (direct analog input)

*   **Left Trigger (LT) → IA_Brake**
    *   Raw axis: 0.0 to 1.0
    *   No modifiers (direct analog input)

*   **Left Stick X-Axis → IA_Steering**
    *   Raw axis: -1.0 to 1.0
    *   Modifiers:
        *   Dead Zone (user-configurable, default 10%)
        *   Sensitivity Scaling (user-configurable, default 1.0x)
        *   Linearity Curve (user-configurable, default 0.0 = linear)

*   **Right Stick Click → IA_LookBack**
    *   Trigger: Pressed

*   **Y / Triangle → IA_ChangeCamera**
    *   Trigger: Pressed

*   **Start Button → IA_PauseMenu**
    *   Trigger: Pressed

**Notes:**
*   Triggers provide natural analog input for throttle/brake
*   Dead-zone prevents stick drift from affecting steering
*   Linearity curve allows more precise center steering

---

### IMC_Wheel

**Steering wheel and pedal-specific mappings:**

*   **Steering Wheel Axis → IA_Steering**
    *   Raw axis: User-calibrated min/max range
    *   Modifiers:
        *   Dead Zone (user-configurable, default 5%)
        *   Sensitivity Scaling (user-configurable, default 1.0x)
        *   Linearity Curve (user-configurable, default 0.0 = linear)
        *   Calibration Offset (from calibration wizard)

*   **Throttle Pedal Axis → IA_Throttle**
    *   Raw axis: User-calibrated min/max range
    *   Modifiers:
        *   Calibration Offset and Scale

*   **Brake Pedal Axis → IA_Brake**
    *   Raw axis: User-calibrated min/max range
    *   Modifiers:
        *   Calibration Offset and Scale

*   **Clutch Pedal Axis** (Optional, Future)
    *   Raw axis: User-calibrated min/max range
    *   Reserved for manual transmission

*   **Paddle Shifters / Buttons → IA_ShiftUp / IA_ShiftDown** (Future)
    *   Trigger: Pressed

**Notes:**
*   Requires calibration wizard on first use
*   Supports DirectInput and XInput force feedback wheels
*   Calibration handles varying axis ranges across different wheel models

---

## Input Processing Pipeline

### 1. Hardware Input Detection

*   UE's input system detects raw device input:
    *   Keyboard: Key press/release events
    *   Gamepad: XInput/DirectInput analog axes and buttons
    *   Wheel: Generic HID device or DirectInput force feedback device

### 2. Mapping Context Selection

*   `PlayerController` activates appropriate Input Mapping Context based on user's selected device in Settings
*   Only one context active at a time
*   Context switch triggers when user changes device in Settings

### 3. Input Modifiers Applied

Applied in order:

1. **Dead-Zone Filtering** (for analog inputs)
    *   Ignores input below threshold (prevents stick drift)
    *   Rescales remaining range to 0.0-1.0

2. **Calibration Offset/Scale** (for wheel/pedals)
    *   Applies user-calibrated min/max values
    *   Normalizes to 0.0-1.0 range

3. **Sensitivity Scaling** (user-configurable multiplier)
    *   Multiplies input value by sensitivity setting (0.5x - 2.0x)

4. **Linearity Curve** (for steering)
    *   0.0 = Linear response (output = input)
    *   1.0 = Exponential curve (more precision near center)
    *   Formula: `output = sign(input) * pow(abs(input), 1 + linearity)`

5. **Digital-to-Analog Conversion** (for keyboard)
    *   Smoothly ramps value from 0 to 1 (or 1 to 0) over time
    *   Simulates analog behavior from digital input

### 4. Action Triggering

*   Enhanced Input System evaluates triggers (Pressed, Released, Held, etc.)
*   Fires bound action with processed value
*   `PlayerController` receives action event

### 5. PlayerController Processing

Each frame (in `Tick` or input event handlers):

1. Read current values of `IA_Throttle`, `IA_Brake`, `IA_Steering`
2. Clamp values to valid ranges (0-1 for throttle/brake, -1 to 1 for steering)
3. Package into `ClientMessage::PlayerInput` struct:
    ```cpp
    struct PlayerInput {
        float throttle;    // 0.0 - 1.0
        float brake;       // 0.0 - 1.0
        float steering;    // -1.0 - 1.0
        uint64_t server_tick_ack;  // Last received server tick
    };
    ```

### 6. Network Transmission

*   `SimNetClient` sends `PlayerInput` via UDP at **240Hz** (every 4.16ms)
*   Packet size: ~16 bytes (minimal bandwidth usage)
*   Includes `server_tick_ack` for server-side latency calculation
*   No delivery guarantee (UDP) - acceptable for high-frequency input

---

## Force Feedback (FFB)

### Supported Devices

*   **DirectInput FFB Wheels:** Logitech G27/G29/G920/G923, Thrustmaster T150/T300/TX, Fanatec CSL/DD
*   **XInput FFB:** Limited Xbox 360/One controller rumble (basic vibration only)

### FFB Data Source

Two potential sources:

1. **Server-Sent Forces:** Server includes FFB data in `Telemetry` packets
    *   Lateral G-force (for resistance in turns)
    *   Road surface vibration (future)
    *   Collision impulse
2. **Client-Calculated Forces:** Client derives forces from `FClientCarState`
    *   Calculate lateral force from yaw rate and speed
    *   Simpler, lower latency, but less accurate

### FFB Processing

1. **PlayerController** reads FFB strength setting (0-100%) from user settings
2. Calculates force magnitude from lateral G-force or collision data
3. Scales force by user's FFB strength percentage
4. Translates into DirectInput constant force effect:
    ```cpp
    DICONSTANTFORCE constantForce;
    constantForce.lMagnitude = forceMagnitude * ffbStrength;
    ```
5. Applies effect to steering wheel device via platform API (Windows: DirectInput, Linux: SDL2 or native)

### FFB Effects

*   **Steering Resistance:** Constant force proportional to lateral G (harder to turn at high speed)
*   **Centering Spring:** Weak spring force to center wheel (active at low speed)
*   **Road Bumps (Future):** Periodic force for track surface texture
*   **Collision Feedback:** Impulse force spike on collision

### Fallback Behavior

*   If FFB device not detected: Skip FFB processing (wheel operates in passive mode)
*   If FFB disabled in settings: Zero all force magnitudes
*   If platform doesn't support FFB: Log warning, continue without FFB

---

## Input Device Hot-Swapping

### Device Connection/Disconnection Detection

*   UE's input system fires events on device add/remove
*   `PlayerController` listens for these events

### Handling Disconnection

1. **Detection:** Active input device disconnected (e.g., wheel unplugged)
2. **Notification:** Display on-screen message:
    ```
    "Input device disconnected. Press any key/button to continue with [fallback device]."
    ```
3. **Graceful Fallback:**
    *   If wheel disconnected: Fall back to keyboard or gamepad (whichever responds first)
    *   If gamepad disconnected: Fall back to keyboard
4. **Input Mapping Context Switch:** Activate fallback device's Input Mapping Context

### Handling Reconnection

1. **Detection:** Previously active device reconnected
2. **Prompt:** Show message: "Device [name] reconnected. Switch to this device?"
3. **User Choice:**
    *   If yes: Switch Input Mapping Context back to reconnected device
    *   If no: Continue with current device

### Settings Persistence

*   Device changes during gameplay are temporary
*   Permanent device changes only saved when user explicitly saves in Settings menu
*   On next launch, use last-saved device preference

---

## Accessibility Considerations

### Rebindable Controls

*   **All Input Actions Rebindable:** Users can map any action to any key/button
*   **Rebinding UI:** In Settings > Controls, click binding to enter rebind mode
*   **Conflict Detection:** Warn if key/button already bound to another action
*   **Reset to Defaults:** One-click reset per device type

### Alternate Input Methods

*   **One-Handed Play:** All actions can be bound to one side of keyboard or gamepad
*   **Configurable Sensitivity:** Separate sensitivity for steering, throttle, brake
    *   Useful for users with limited range of motion
*   **Reduced Precision Mode:** Higher linearity curves for easier control at lower skill levels

### Toggle vs Hold

*   **Look Back:** Future option to toggle rear-view instead of hold
*   **Clutch (Future):** Option for auto-clutch or manual clutch

### Colorblind Support

*   Input display widgets (throttle/brake bars in HUD) use shapes in addition to colors
*   Future: Configurable color themes for input indicators

---

## Performance Considerations

### Input Sampling Rate

*   **Keyboard:** Polled at OS rate (~125-1000Hz), processed at UE frame rate (60-240Hz)
*   **Gamepad:** Polled at controller's update rate (~125Hz XInput, ~1000Hz some DirectInput)
*   **Wheel:** Polled at device rate (~1000Hz for high-end wheels, ~125Hz for basic wheels)

### Network Bandwidth

*   **Input Send Rate:** 240Hz (every ~4ms)
*   **Packet Size:** 16 bytes (3 floats + 1 uint64)
*   **Bandwidth:** ~30 Kbps upload (very low)

### CPU Usage

*   Input processing is lightweight (<0.1ms per frame)
*   FFB calculations minimal (<0.05ms per frame)
*   No performance impact from Enhanced Input System modifiers

---

## Future Enhancements

*   **Input Replay:** Record and replay input sequences for testing
*   **Input Display:** On-screen overlay showing live input values (for tutorials/streaming)
*   **Advanced FFB:** Tire slip feedback, engine vibration
*   **Button Boxes:** Support for custom USB button boxes (via DirectInput/HID)
*   **Motion Platforms:** Output for motion simulators (via plugins)
*   **VR Controllers:** Support for VR hand tracking and controllers
