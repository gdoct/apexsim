# Settings Scene Specification

## Overview

The Settings scene provides comprehensive configuration options for graphics, audio, controls, network, and gameplay. All settings are client-side and persist across sessions.

---

## Scene Type

*   **Implementation:** UMG Widget Blueprint with tabbed interface
*   **Server Interaction:** None (all client-side configuration)
*   **Persistence:** Settings stored in user configuration file

---

## Configuration File

*   **Windows:** `%AppData%/ApexSim/Config/UserSettings.ini`
*   **Linux:** `~/.config/ApexSim/UserSettings.ini`
*   **Format:** INI format with sections for each settings category
*   **Auto-save:** On "Apply" or "Save" button click
*   **Backup:** Create backup on first launch (for factory reset)

---

## UI Layout

### Tab Navigation

Horizontal tab bar at the top with the following tabs:
1. Graphics
2. Audio
3. Controls
4. Network
5. Gameplay

### Common Elements

*   **Save Button:** Apply changes and return to previous scene
*   **Apply Button:** Apply changes without closing settings
*   **Reset to Defaults Button:** Reset current tab to default values (with confirmation)
*   **Back Button:** Return without saving (confirm if changes made)

---

## Graphics Tab

### Display Settings

#### Resolution (Dropdown)
*   **Options:** All supported resolutions (e.g., 1920x1080, 2560x1440, 3840x2160)
*   **Detection:** Auto-detect monitor's native resolution
*   **Default:** Native resolution

#### Display Mode (Dropdown)
*   **Options:**
    *   Fullscreen
    *   Borderless Windowed
    *   Windowed
*   **Default:** Fullscreen

#### VSync (Toggle)
*   **Options:** On / Off
*   **Default:** On
*   **Note:** May increase input latency when enabled

#### Frame Rate Limit (Dropdown)
*   **Options:** Unlimited, 60, 120, 144, 240 FPS
*   **Default:** Unlimited
*   **Conditional:** Grayed out if VSync is On

### Graphics Quality

#### Graphics Preset (Dropdown)
*   **Options:** Low, Medium, High, Ultra, Custom
*   **Behavior:**
    *   Selecting a preset applies predefined values to all advanced settings
    *   Manually changing any advanced setting switches preset to "Custom"
*   **Default:** High

#### Advanced Settings

Visible when preset is "Custom" (or expanded section):

*   **View Distance (Slider)**
    *   Range: Low (1) to Epic (5)
    *   Default: 4
    *   Effect: Render distance for objects and terrain

*   **Anti-Aliasing (Dropdown)**
    *   Options: Off, FXAA, TAA, TSR (Temporal Super Resolution)
    *   Default: TAA
    *   Effect: Edge smoothing quality

*   **Shadows Quality (Slider)**
    *   Range: Low (1) to Epic (5)
    *   Default: 4
    *   Effect: Shadow resolution and distance

*   **Textures Quality (Slider)**
    *   Range: Low (1) to Epic (5)
    *   Default: 4
    *   Effect: Texture resolution and streaming

*   **Effects Quality (Slider)**
    *   Range: Low (1) to Epic (5)
    *   Default: 4
    *   Effect: Particle systems, tire smoke, etc.

*   **Post-Processing (Slider)**
    *   Range: Low (1) to Epic (5)
    *   Default: 4
    *   Effect: Bloom, motion blur, color grading

*   **Foliage Quality (Slider)**
    *   Range: Low (1) to Epic (5)
    *   Default: 3
    *   Effect: Trackside vegetation density

### Implementation Notes

*   Changes trigger UE's `ApplySettings()` console commands
*   Graphics changes may require brief loading screen
*   Show estimated VRAM usage for current settings
*   Warn if settings exceed detected hardware capabilities

---

## Audio Tab

### Volume Controls

All volume sliders use range 0-100%, default 75%:

*   **Master Volume:** Overall audio level
*   **Engine Volume:** Car engine sounds
*   **Tire Volume:** Tire rolling and screeching sounds
*   **Ambient Volume:** Environmental sounds (wind, crowd, etc.)
*   **UI Volume:** Menu sounds and notifications

### Audio Device

*   **Audio Output Device (Dropdown)**
    *   Options: System default + all detected audio devices
    *   Default: System default
    *   Hot-swap support: Update list if devices change

### Audio Quality (Future)

*   Sample rate options (44.1kHz, 48kHz)
*   Spatial audio options (Stereo, 5.1, 7.1, Dolby Atmos)

---

## Controls Tab

### Input Device Selection

*   **Input Device (Dropdown)**
    *   Options: Keyboard, Gamepad, Steering Wheel + Pedals
    *   Default: Keyboard
    *   Behavior: Switching device changes active Input Mapping Context

### Keyboard Bindings

Rebindable keys for the following actions:

*   **Throttle:** W (default)
*   **Brake:** S (default)
*   **Steering Left:** A (default)
*   **Steering Right:** D (default)
*   **Pause/Menu:** ESC (default)
*   **Look Back:** C (default)
*   **Change Camera:** V (default)

**Rebinding Process:**
1. Click on binding field
2. Display "Press any key..."
3. Capture next key press
4. Update binding
5. Check for conflicts, show warning if detected

### Gamepad Bindings

Rebindable gamepad inputs:

*   **Throttle:** Right Trigger (RT) (default)
*   **Brake:** Left Trigger (LT) (default)
*   **Steering:** Left Stick X-Axis (default)
*   **Pause/Menu:** Start Button (default)
*   **Look Back:** Right Stick Click (default)
*   **Change Camera:** Y / Triangle (default)

**Dead-zone Slider:** 0-20%, default 10%

### Wheel/Pedals Bindings

*   **Auto-Detection:** Automatically detect connected steering wheels and pedals
*   **Calibration Wizard:**
    1. Turn wheel fully left → Capture min value
    2. Center wheel → Capture center value
    3. Turn wheel fully right → Capture max value
    4. Press throttle pedal fully → Capture throttle range
    5. Press brake pedal fully → Capture brake range
    6. Press clutch pedal fully (optional) → Capture clutch range

**Configuration:**
*   **Steering Axis** with dead-zone slider (0-20%, default 5%)
*   **Throttle Axis** with calibrated min/max
*   **Brake Axis** with calibrated min/max
*   **Clutch Axis** (optional) with calibrated min/max
*   **Gear Up / Gear Down** buttons (optional, for manual transmission)
*   **Force Feedback Strength:** 0-100%, default 80%

### Steering Adjustments

*   **Steering Sensitivity (Slider)**
    *   Range: 0.5x - 2.0x
    *   Default: 1.0x
    *   Effect: Multiplier for steering input

*   **Steering Linearity (Slider)**
    *   Range: 0.0 - 1.0
    *   Default: 0.0 (linear)
    *   Effect: 0 = linear, 1 = exponential curve (more precise at center)

### Implementation Notes

*   Changes reload active Input Mapping Context
*   Wheel calibration wizard runs on first wheel detection
*   Store separate profiles for each device type
*   Validate FFB device support before showing FFB options

---

## Network Tab

### Server Connection

*   **Server Address (Text Input)**
    *   Default: "localhost" or last connected server
    *   Validation: IP address or hostname format
    *   Placeholder: "Enter IP or hostname"

*   **TCP Port (Number Input)**
    *   Range: 1024-65535
    *   Default: 9000

*   **UDP Port (Number Input)**
    *   Range: 1024-65535
    *   Default: 9001

*   **Connection Timeout (Slider)**
    *   Range: 5-30 seconds
    *   Default: 10 seconds

### Network Diagnostics

*   **Show Network Stats (Toggle)**
    *   Default: Off
    *   Effect: Displays ping, packet loss, and tick rate in HUD

### Future Features

*   Server favorites list
*   Auto-connect to last server
*   Port forwarding test utility

---

## Gameplay Tab

### Player Profile

*   **Player Name (Text Input)**
    *   Length: 3-20 characters
    *   Validation: Alphanumeric + spaces, no profanity filter (server-side)
    *   Default: "Player" + random number

### HUD Customization

*   **HUD Opacity (Slider)**
    *   Range: 0-100%
    *   Default: 80%

*   **HUD Scale (Slider)**
    *   Range: 0.5x - 1.5x
    *   Default: 1.0x

*   **Speedometer Units (Dropdown)**
    *   Options: km/h, mph, m/s
    *   Default: km/h

### Driving Assists

*   **Racing Line (Dropdown)**
    *   Options: Off, Braking Only, Full Line
    *   Default: Braking Only

*   **Traction Control (Dropdown)**
    *   Options: Off, Low, Medium, High
    *   Default: Medium

*   **ABS (Toggle)**
    *   Default: On

*   **Stability Control (Toggle)**
    *   Default: On

### Camera Settings

*   **Camera FOV (Slider)**
    *   Range: 60-120 degrees
    *   Default: 90 degrees

*   **Camera Shake (Slider)**
    *   Range: 0-100%
    *   Default: 50%

### Implementation Notes

*   Player name syncs with server on change
*   Driving assists send configuration to server (future server-side validation)
*   Camera settings apply immediately to active camera

---

## Behavior & Logic

### Applying Settings

*   **Apply Button:** Applies settings without closing menu
*   **Save Button:** Applies settings and returns to previous scene
*   **Real-time Preview:** Some settings (FOV, camera shake) preview live in background

### Resetting Settings

*   **Reset to Defaults:**
    1. Show confirmation dialog
    2. Reset current tab to factory defaults
    3. Auto-apply changes
    4. Show "Settings reset" confirmation

### Validation

*   Validate all inputs before applying
*   Show error messages for invalid values
*   Highlight invalid fields in red
*   Prevent saving until all fields valid

### Unsaved Changes

*   Track if any changes made since last save
*   Show confirmation dialog on "Back" if unsaved: "You have unsaved changes. Discard?"

---

## Accessibility

*   **Keyboard Navigation:** Full tab and arrow key support
*   **Controller Navigation:** D-pad navigation through all fields
*   **Screen Reader:** Announce slider values and dropdown selections
*   **High Contrast:** Support for high-contrast text in all tabs

---

## Performance Considerations

*   Minimize settings widget complexity (lazy-load tabs)
*   Batch graphics setting changes (apply all at once)
*   Cache dropdown options (don't regenerate on every open)
