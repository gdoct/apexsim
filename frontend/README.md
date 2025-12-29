
## SimRacing Frontend Application Specification (Unreal Engine) - Initial Phase

**Project Goal:** Develop a responsive Unreal Engine client application capable of connecting to the Rust backend server, displaying game menus, rendering a basic driving view based on server telemetry, and sending player input. Designed for cross-platform compatibility and future extensibility.

**Engine:** Unreal Engine 5.3+ (latest stable UE version recommended)
**Target Platforms:**
*   **Primary:** Windows 10/11 (64-bit) - DirectX 12, Vulkan
*   **Secondary:** Linux (Ubuntu 22.04+, Fedora 38+) - Vulkan
*   **Future:** macOS (Metal), Steam Deck (Proton/Vulkan)
*   **Minimum Requirements:**
    *   CPU: 4-core @ 2.5GHz (Intel i5-8400 / AMD Ryzen 5 2600 equivalent)
    *   GPU: 4GB VRAM, DX12/Vulkan support (GTX 1060 / RX 580 equivalent)
    *   RAM: 8GB
    *   Storage: 10GB SSD space
    *   Network: Broadband connection (5 Mbps down, 1 Mbps up, <100ms ping)

---

### 1. High-Level Architecture

The frontend will be a standard Unreal Engine project. Core networking and game state interpretation will be handled in C++ for performance, while UI and scene management can leverage a mix of C++ and Blueprints for rapid iteration.

**Core Components (UE Modules/Plugins/Classes):**

*   **`GameInstance` / `GameMode`:** Overall game state management and rules.
*   **`PlayerController`:** Handles player input and client-side logic.
*   **`SimNetClient` (C++):** Custom networking module to communicate with the Rust server.
*   **`PlayerCar` / `OtherCar` (C++ / Blueprint):** Base classes for car rendering and visual logic.
*   **`TrackActor` (C++ / Blueprint):** Base class for track rendering.
*   **UI (UMG):** User interfaces for menus, HUD, and content management.

---

### 2. Core Data Structures (C++ Classes / Blueprints)

The client will mirror relevant server data structures, adapting them for UE's environment.

```cpp
// In a dedicated C++ class, e.g., USimGameData (subclass of UObject) or similar
// These should ideally be USTRUCTs to leverage UE's reflection system where appropriate
// and be easily accessible in Blueprints.

// --- Identifiers (Matching Rust backend) ---
// Using FGuid for UE's UUID equivalent
typedef FGuid FPlayerId;
typedef FGuid FSessionId;
typedef FGuid FCarConfigId;
typedef FGuid FTrackConfigId;

// --- Player (Client's view of a player) ---
USTRUCT(BlueprintType)
struct FClientPlayer
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FPlayerId Id;
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FString Name;
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FCarConfigId SelectedCarConfigId;
    // Add other relevant player details received from server
};

// --- Car Configuration (Client's view of a car model) ---
// Used for displaying car names in menus, and potentially loading correct visual models.
USTRUCT(BlueprintType)
struct FClientCarConfig
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FCarConfigId Id;
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FString Name;
    // Later: path to mesh, materials, textures for this car config
};

// --- Track Configuration (Client's view of a track) ---
// Used for displaying track names in menus, and potentially loading correct visual models.
USTRUCT(BlueprintType)
struct FClientTrackConfig
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FTrackConfigId Id;
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FString Name;
    // Later: path to mesh, materials, textures for this track config
};

// --- Car Dynamics State (Received from Server Telemetry) ---
// This is the core data used to render cars.
USTRUCT(BlueprintType)
struct FClientCarState
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FPlayerId PlayerId;
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FCarConfigId CarConfigId;
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FVector2D Position; // X, Y from server (UE's FVector2D)
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float YawRad;       // Orientation from server (radians)
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float SpeedMps;     // Speed for HUD
    // Add other relevant telemetry (lap, best lap, current lap time, etc.)
};

// --- Race Session State (Client's view of a session) ---
USTRUCT(BlueprintType)
struct FClientRaceSession
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FSessionId Id;
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FTrackConfigId TrackConfigId;
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TMap<FPlayerId, FClientPlayer> ConnectedPlayers; // Simplified, mapping player ID to basic player info
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    ERaceSessionState State; // Enum matching server's SessionState
    // Add max_players, lap_limit, etc.
};
```

---

### 3. Network Module (`SimNetClient` C++ Class)

This custom C++ class (likely a `UObject` or a `GameInstance` subclass component) will handle all communication with your Rust backend.

*   **Initialization:** Connects UDP/TCP sockets to the server.
*   **Serialization/Deserialization:** Converts UE data structures to/from the Rust server's message formats (e.g., using `bincode` or `flatbuffers` in C++ for performance).
*   **Sending Input:** Packages `PlayerInput` data (throttle, brake, steering) and sends it via UDP at high frequency.
*   **Receiving Telemetry:**
    *   Listens for UDP `Telemetry` packets.
    *   Deserializes `Telemetry` data into `FClientCarState` structs.
    *   Manages client-side prediction and interpolation for smooth rendering (see `Driving View` below).
    *   Publishes events (e.g., "OnTelemetryReceived") or updates game state.
*   **Receiving Lobby/Session Updates:**
    *   Listens for TCP `ServerMessage`s (LobbyUpdate, SessionCreated, etc.).
    *   Updates client's internal representation of available players, sessions, cars, and tracks.
    *   Publishes events (e.g., "OnLobbyUpdated") for UI elements to react to.

---

### 4. Unreal Engine Scenes / Levels

#### 4.1. `MainMenu` Scene (Level / Widget Blueprint)

*   **Type:** Primarily UMG Widget Blueprint. Can be loaded as a Level or be the default startup widget.
*   **Components:**
    *   **"Play" Button:** Navigates to the Create/Join Session screen.
    *   **"Settings" Button:** Navigates to the Settings screen.
    *   **"Content" Button:** Navigates to the Content Management screen.
    *   **"Quit" Button:** Exits the application.
*   **Logic:** Simple navigation. No direct server interaction here, just UI flow.

#### 4.2. `CreateJoinSession` Scene (Level / Widget Blueprint)

*   **Type:** Primarily UMG Widget Blueprint.
*   **Components:**
    *   **Lobby Player List:** Displays names of players currently in the lobby, received via `SimNetClient`'s `LobbyUpdate`.
    *   **Available Sessions List:** Displays `SessionId`, `TrackName`, `HostName`, `Players (X/Y)`, `State`, received via `LobbyUpdate`.
    *   **"Create Session" Button:**
        *   Opens a small popup for `Track Selection`, `Max Players`, `Lap Limit`.
        *   Sends `ClientMessage::CreateSession` via `SimNetClient`.
    *   **"Join Session" Buttons:** For each listed session, sends `ClientMessage::JoinSession`.
    *   **"Back" Button:** Returns to `MainMenu`.
*   **Logic:** Reacts to `SimNetClient`'s "OnLobbyUpdated" event to populate lists. Handles button presses by sending `ClientMessage`s.

#### 4.3. `Settings` Scene (Level / Widget Blueprint)

*   **Type:** Primarily UMG Widget Blueprint.
*   **Components:**
    *   **Graphics Tab:**
        *   Resolution (dropdown: 1920x1080, 2560x1440, 3840x2160, etc.)
        *   Display Mode (dropdown: Fullscreen, Borderless Windowed, Windowed)
        *   VSync (toggle: On/Off)
        *   Frame Rate Limit (dropdown: Unlimited, 60, 120, 144, 240)
        *   Graphics Preset (dropdown: Low, Medium, High, Ultra, Custom)
        *   Advanced Settings (if Custom selected):
            *   View Distance (slider: Low to Epic)
            *   Anti-Aliasing (dropdown: Off, FXAA, TAA, TSR)
            *   Shadows Quality (slider: Low to Epic)
            *   Textures Quality (slider: Low to Epic)
            *   Effects Quality (slider: Low to Epic)
            *   Post-Processing (slider: Low to Epic)
            *   Foliage Quality (slider: Low to Epic)
    *   **Audio Tab:**
        *   Master Volume (slider: 0-100%)
        *   Engine Volume (slider: 0-100%)
        *   Tire Volume (slider: 0-100%)
        *   Ambient Volume (slider: 0-100%)
        *   UI Volume (slider: 0-100%)
        *   Audio Output Device (dropdown: system default + detected devices)
    *   **Controls Tab:**
        *   Input Device (dropdown: Keyboard, Gamepad, Steering Wheel + Pedals)
        *   **Keyboard Bindings** (rebindable):
            *   Throttle: W (or configurable to axis via modifier keys)
            *   Brake: S
            *   Steering Left: A
            *   Steering Right: D
            *   Pause/Menu: ESC
            *   Look Back: C
            *   Change Camera: V
        *   **Gamepad Bindings** (rebindable):
            *   Throttle: Right Trigger (RT)
            *   Brake: Left Trigger (LT)
            *   Steering: Left Stick X-Axis
            *   Pause/Menu: Start Button
            *   Look Back: Right Stick Click
            *   Change Camera: Y / Triangle
        *   **Wheel/Pedals Bindings** (auto-detected, calibration wizard):
            *   Steering Axis (with dead-zone slider: 0-20%)
            *   Throttle Axis (with calibration min/max)
            *   Brake Axis (with calibration min/max)
            *   Clutch Axis (optional, with calibration min/max)
            *   Gear Up / Gear Down (optional buttons for manual transmission)
            *   Force Feedback Strength (slider: 0-100%, if supported)
        *   Steering Sensitivity (slider: 0.5x - 2.0x, affects steering input scaling)
        *   Steering Linearity (slider: 0.0 - 1.0, 0 = linear, 1 = exponential curve)
    *   **Network Tab:**
        *   Server Address (text input: IP or hostname)
        *   TCP Port (number input: default 9000)
        *   UDP Port (number input: default 9001)
        *   Connection Timeout (slider: 5-30 seconds)
        *   Show Network Stats (toggle: displays ping, packet loss in HUD)
    *   **Gameplay Tab:**
        *   Player Name (text input: 3-20 characters)
        *   HUD Opacity (slider: 0-100%)
        *   HUD Scale (slider: 0.5x - 1.5x)
        *   Speedometer Units (dropdown: km/h, mph, m/s)
        *   Enable Assists (toggle group):
            *   Racing Line (toggle: Off, Braking Only, Full Line)
            *   Traction Control (dropdown: Off, Low, Medium, High)
            *   ABS (toggle: On/Off)
            *   Stability Control (toggle: On/Off)
        *   Camera FOV (slider: 60-120 degrees)
        *   Camera Shake (slider: 0-100%)
    *   **"Save" / "Apply" / "Reset to Defaults" / "Back" Buttons.**
*   **Logic:**
    *   Settings are stored in `%AppData%/ApexSim/Config/UserSettings.ini` (Windows) or `~/.config/ApexSim/UserSettings.ini` (Linux)
    *   "Apply" button applies changes immediately without closing the menu
    *   "Save" button applies and returns to main menu
    *   Changes to graphics settings trigger UE's `ApplySettings()` console commands
    *   Input device changes reload the active input mapping context
    *   Wheel calibration wizard: prompt user to turn wheel fully left, center, fully right, press pedals fully to detect ranges
    *   No server interaction (all client-side configuration)

#### 4.4. `ContentManagement` Scene (Level / Widget Blueprint)

*   **Type:** Primarily UMG Widget Blueprint.
*   **Components:**
    *   **"My Cars" List:** Displays `CarConfig` names from `SimNetClient` (initially server-provided, later local mods).
    *   **"My Tracks" List:** Displays `TrackConfig` names.
    *   **"Select Car" Button:** For the current player in the lobby, sends `ClientMessage::SelectCar`.
    *   **"Back" Button:** Returns to `MainMenu`.
*   **Logic:** Reacts to `SimNetClient`'s "OnLobbyUpdated" event to display available content. Placeholder for future local mod loading.

#### 4.5. `DrivingView` Scene (Level)

*   **Type:** Full 3D Level.
*   **Components:**
    *   **`PlayerCar` Actor:** Spawns a basic car mesh for the local player.
        *   **Camera System:** See Section 7 for detailed camera specifications
        *   **Input Component:** Reads local player input (steering, throttle, brake) and sends it via `SimNetClient`.
        *   **Client-side Prediction/Interpolation:** Crucial for smoothness. The `PlayerCar` will predict its movement locally between server updates. When a server update arrives, it will reconcile its predicted state with the authoritative server state, interpolating smoothly to correct any drift.
        *   **Visual Effects:**
            *   Wheel rotation animation (driven by speed)
            *   Steering wheel animation (driven by steering input)
            *   Suspension compression visualization (basic spring offset)
            *   Tire smoke particles (on hard braking or collision)
            *   Damage/dirt overlay (future feature)
    *   **`OtherCar` Actors:** Spawns basic car meshes for all other players in the session.
        *   These only render the car's visual model. Their movement is purely driven by `Telemetry` received from the server, using interpolation between received `FClientCarState` updates for smoothness.
        *   Same visual effects as `PlayerCar` (wheel rotation, smoke, etc.)
        *   Nameplate widget above car showing player name and position
    *   **`TrackActor`:** Spawns a basic 3D track mesh (initially a flat plane with texture).
        *   Track boundaries (invisible collision volumes for out-of-bounds detection)
        *   Start/finish line visual (checkered pattern, timing gate)
        *   Pit lane markers (future feature)
    *   **`HUD` Widget (UMG):** See Section 8 for detailed HUD specifications
*   **Logic:**
    *   `GameMode` loads the specific `TrackActor` based on `Session` data.
    *   `SimNetClient` continuously updates `FClientCarState` data for all cars in the session.
    *   `PlayerCar` and `OtherCar` actors use this `FClientCarState` (and client-side prediction for the local player) to update their 3D positions and rotations.
    *   Countdown timer displays "5... 4... 3... 2... 1... GO!" during Countdown state
    *   Session state determines available actions (can't pause during countdown, can't join during race, etc.)

---

### 5. Input Management

Unreal Engine 5's Enhanced Input System is used for all input handling, providing flexible and rebindable controls across multiple device types.

#### 5.1. Input Actions

Defined in `Content/Input/IA_*.uasset`:

*   **`IA_Throttle`** (Value, Axis1D): Range 0.0 to 1.0
*   **`IA_Brake`** (Value, Axis1D): Range 0.0 to 1.0
*   **`IA_Steering`** (Value, Axis1D): Range -1.0 (left) to 1.0 (right)
*   **`IA_LookBack`** (Button): Toggle rear-view camera
*   **`IA_ChangeCamera`** (Button): Cycle through camera views
*   **`IA_PauseMenu`** (Button): Open/close pause menu
*   **`IA_ShiftUp`** (Button): Manual transmission shift up (future)
*   **`IA_ShiftDown`** (Button): Manual transmission shift down (future)

#### 5.2. Input Mapping Contexts

Defined in `Content/Input/IMC_*.uasset`:

**`IMC_Keyboard`:**
*   W → IA_Throttle (with "Digital to Analog" modifier, ramp-up time 0.1s)
*   S → IA_Brake (with "Digital to Analog" modifier, ramp-up time 0.05s)
*   A → IA_Steering (value -1.0, with "Digital to Analog" modifier, ramp-up time 0.1s)
*   D → IA_Steering (value 1.0, with "Digital to Analog" modifier, ramp-up time 0.1s)
*   C → IA_LookBack
*   V → IA_ChangeCamera
*   ESC → IA_PauseMenu

**`IMC_Gamepad`:**
*   Right Trigger (RT) → IA_Throttle (raw axis, 0.0-1.0)
*   Left Trigger (LT) → IA_Brake (raw axis, 0.0-1.0)
*   Left Stick X-Axis → IA_Steering (with dead-zone modifier from settings, sensitivity scaling)
*   Right Stick Click → IA_LookBack
*   Y/Triangle → IA_ChangeCamera
*   Start → IA_PauseMenu

**`IMC_Wheel`:**
*   Steering Wheel Axis → IA_Steering (with user-calibrated min/max, dead-zone, sensitivity, linearity curve)
*   Throttle Pedal Axis → IA_Throttle (with user-calibrated min/max)
*   Brake Pedal Axis → IA_Brake (with user-calibrated min/max)
*   Clutch Pedal Axis → (Reserved for future manual transmission)
*   Paddle Shifters / Sequential Buttons → IA_ShiftUp / IA_ShiftDown

#### 5.3. Input Processing Pipeline

1. **Hardware Input:** UE's input system detects raw device input (keyboard, XInput/DirectInput gamepad, or generic HID device for wheels)
2. **Mapping Context Selection:** `PlayerController` activates the appropriate `IMC_*` based on the user's selected input device in Settings
3. **Modifiers Applied:**
    *   Dead-zone filtering (for analog sticks and wheel axes)
    *   Sensitivity scaling (multiplier from settings)
    *   Linearity curve (exponential response curve for steering)
    *   Digital-to-analog conversion (smooth ramp for keyboard inputs)
4. **Action Triggering:** Enhanced Input System triggers bound actions (e.g., `IA_Throttle` fires with current value)
5. **PlayerController Processing:**
    *   Reads triggered action values each frame
    *   Clamps values to valid ranges (0-1 for throttle/brake, -1 to 1 for steering)
    *   Packages values into `ClientMessage::PlayerInput` struct
6. **Network Transmission:** `SimNetClient` sends `PlayerInput` via UDP at **240Hz** (matching server tick rate)
    *   Includes `server_tick_ack` (last received server tick) for server-side latency calculation
    *   Small packet size (~16 bytes) for minimal bandwidth usage

#### 5.4. Force Feedback (FFB)

*   **Supported Devices:** DirectInput and XInput-compatible force feedback wheels (Logitech G29/G920, Thrustmaster T300/TX, Fanatec CSL/DD, etc.)
*   **FFB Source:** Server sends simplified force data in `Telemetry` packets (or client calculates locally from car state):
    *   Lateral G-force (for wheel resistance in turns)
    *   Road surface vibration (from track surface type, future feature)
    *   Collision feedback (impulse on collision)
*   **Client-Side FFB Processing:**
    *   `PlayerController` reads FFB strength setting (0-100%) from user settings
    *   Translates forces into DirectInput/XInput constant force effects
    *   Applies effects to steering wheel device via platform APIs
*   **Fallback:** If FFB device not detected or disabled in settings, no force feedback is applied (wheel operates in passive mode)

#### 5.5. Input Device Hot-Swapping

*   **Detection:** UE's input system monitors for device connection/disconnection events
*   **Notification:** If the active input device is disconnected (e.g., wheel unplugged), display on-screen notification: "Input device disconnected. Press any key/button to continue with [fallback device]."
*   **Graceful Fallback:** Automatically switch to keyboard or gamepad if wheel disconnects mid-session
*   **Settings Sync:** Device changes persist to config file only when user explicitly saves in Settings menu

#### 5.6. Accessibility Considerations

*   **Rebindable Controls:** All input actions are rebindable via the Settings > Controls menu
*   **Alternate Input Methods:** Support for one-handed play via fully rebindable keys
*   **Configurable Sensitivity:** Separate sensitivity sliders for steering, throttle, and brake (useful for limited range-of-motion users)
*   **Toggle vs Hold:** Option to toggle Look Back instead of hold (future feature)
*   **Colorblind HUD:** HUD elements use shapes/icons in addition to colors (future feature)

---

### 6. Camera System

The camera system provides multiple viewpoints for the player, each optimized for different driving styles and preferences. Cameras are switchable via the `IA_ChangeCamera` input action.

#### 6.1. Camera Types

**1. Chase Camera (Default)**
*   **Position:** 4-6 meters behind car, 1.5-2 meters above ground
*   **Behavior:**
    *   Follows car with spring arm smoothing (lag: 0.3s)
    *   Looks at car's center of mass with slight forward offset (1 meter ahead)
    *   Auto-levels to horizon (pitch smoothing to keep horizon straight)
    *   Dynamically adjusts distance based on speed (closer at low speed, farther at high speed)
*   **FOV:** User-configurable (default: 90 degrees)
*   **Collision:** Spring arm retracts if obstructed by track objects

**2. Cockpit Camera (First-Person)**
*   **Position:** Driver's eye position inside car (defined per car model)
*   **Behavior:**
    *   Fixed to car interior, no smoothing (1:1 with car rotation for immersion)
    *   Includes visible steering wheel, dashboard, and interior elements
    *   Head bobbing effect (subtle, user-configurable shake amount)
    *   Look-to-apex on turn-in (optional, slight camera yaw toward apex)
*   **FOV:** User-configurable (default: 75 degrees for narrower interior view)
*   **Dashboard:** Functional gauges (speedo, tachometer, gear indicator)

**3. Hood/Bonnet Camera**
*   **Position:** Front of car, just above hood, centered
*   **Behavior:**
    *   Fixed to car, minimal smoothing
    *   No interior visible, clear forward view
    *   Slight camera shake tied to suspension movement
*   **FOV:** User-configurable (default: 85 degrees)
*   **Use Case:** Balance between visibility and immersion

**4. Rear-View Camera (Look Back)**
*   **Activation:** Hold/toggle `IA_LookBack` input
*   **Position:** Rear of car, looking backward
*   **Behavior:**
    *   Temporarily overrides current camera while held
    *   Returns to previous camera on release (if toggle mode disabled)
    *   Slight field-of-view narrowing (to simulate focusing on rear)
*   **Use Case:** Check for cars behind, defensive driving

**5. TV Camera (Spectator Mode - Future)**
*   **Position:** Trackside static cameras at key corners/straights
*   **Behavior:**
    *   Automatically switches between static camera positions as player passes
    *   Smooth transitions (cut or slow pan)
    *   Used for replay viewing
*   **FOV:** Wide angle (100-110 degrees)

#### 6.2. Camera Smoothing & Responsiveness

*   **Spring Arm Lag:** Configurable per camera type (chase: 0.3s, cockpit: 0.0s, hood: 0.1s)
*   **Rotation Lag:** Separate lag for camera rotation to prevent jarring movements during sudden steering inputs
*   **Speed-Based FOV:** Optional dynamic FOV that increases slightly at high speed (simulates tunnel vision, max +10 degrees at 250+ km/h)
*   **Collision Detection:** Chase camera uses UE's spring arm collision detection to prevent clipping through track geometry

#### 6.3. Camera Shake

*   **Sources:**
    *   Suspension travel (bumps, kerbs)
    *   Engine vibration (RPM-based, subtle)
    *   Collisions (impulse-based shake)
*   **Intensity:** User-configurable global multiplier (0-100% in Settings)
*   **Implementation:** UE's Camera Shake system with procedural shake patterns

---

### 7. HUD (Heads-Up Display)

The HUD provides essential racing information without obstructing the view. All elements are designed for at-a-glance readability during high-speed racing.

#### 7.1. Core HUD Elements

**Speedometer (Bottom-Left)**
*   **Display:** Large numeric speed value + unit (km/h, mph, or m/s based on user setting)
*   **Style:** Bold sans-serif font, high contrast (white text, dark semi-transparent background)
*   **Additional Info:** Small gear indicator below (currently always "N" for automatic, future: 1-6)

**Tachometer (Bottom-Left, adjacent to speedometer) - Future**
*   **Display:** Arc-style RPM gauge with redline indicator
*   **Current:** Placeholder "RPM: ----" text

**Lap Counter (Top-Right)**
*   **Display:** "Lap X / Y" where X is current lap, Y is lap limit
*   **Highlight:** Changes color when on final lap (white → yellow)

**Lap Timer (Top-Right, below lap counter)**
*   **Current Lap Time:** Live timer showing current lap time (MM:SS.mmm format)
*   **Best Lap Time:** Displayed below current time, prefixed with "Best: " (green if current lap is faster)
*   **Delta Indicator:** Shows +/- time difference from best lap (green if ahead, red if behind) - Future

**Position Indicator (Top-Center)**
*   **Display:** "P1" or "1st" (ordinal position based on track progress)
*   **Style:** Large, prominent, changes color based on position (gold for 1st, silver for 2nd, bronze for 3rd, white for others)

**Minimap (Bottom-Right) - Future**
*   **Display:** Simplified top-down track view with all car positions
*   **Player Car:** Highlighted with distinct color/icon
*   **Zoom:** Auto-scales to show nearby cars or full track (user-configurable)

**Input Display (Bottom-Center) - Optional**
*   **Display:** Visual bars showing throttle, brake, and steering input (0-100% filled bars)
*   **Use Case:** Useful for analyzing driving technique, reviewing replays
*   **Visibility:** Toggleable in Settings (default: off)

#### 7.2. Conditional HUD Elements

**Countdown Timer (Center-Screen)**
*   **Display:** Large "5... 4... 3... 2... 1... GO!" with fade-in/out animations
*   **Visibility:** Only shown during `SessionState::Countdown`
*   **Audio:** Synchronized beep sounds (low pitch for 5-2, high pitch for 1, distinctive sound for GO)

**Network Stats (Top-Left) - Optional**
*   **Display:** Ping (ms), Packet Loss (%), Server Tick (for debugging)
*   **Visibility:** Toggleable in Settings > Network > Show Network Stats
*   **Color Coding:** Green (<50ms), yellow (50-100ms), red (>100ms or >5% packet loss)

**Race Results Overlay (Post-Race)**
*   **Display:** Full-screen overlay showing final standings, lap times, best lap for each player
*   **Visibility:** Shown when `SessionState::Finished`
*   **Actions:** Buttons to "Return to Lobby" or "View Replay" (future)

**Pause Menu Overlay (Mid-Race)**
*   **Display:** Semi-transparent overlay with options: Resume, Settings, Leave Session, Quit
*   **Visibility:** Shown when `IA_PauseMenu` is triggered
*   **Background:** Blurred/darkened game view

#### 7.3. HUD Customization

All HUD elements respect user settings:
*   **HUD Opacity:** Global transparency (0-100%)
*   **HUD Scale:** Global size multiplier (0.5x - 1.5x)
*   **Element Visibility:** Future feature allowing individual HUD elements to be hidden
*   **Color Themes:** Future feature for colorblind-friendly palettes

#### 7.4. Responsive Layout

*   **Aspect Ratios:** HUD elements anchor to screen edges and scale appropriately for 16:9, 21:9 (ultrawide), 16:10, and 4:3 displays
*   **Safe Zones:** Critical information (speed, lap time, position) stays within TV-safe zones for potential console ports
*   **Dynamic Scaling:** Text sizes scale with resolution to maintain readability (larger at 4K, smaller at 1080p)

---

### 8. Audio System

The audio system provides immersive sound feedback for racing, UI interactions, and environmental ambiance.

#### 8.1. Car Audio

**Engine Sounds**
*   **Source:** Multi-layered engine samples at different RPM ranges (idle, low, mid, high, redline)
*   **Playback:** Real-time pitch shifting based on current RPM (derived from speed and gear)
*   **Spatial:** 3D positioned at car's engine location
*   **Player Car:** Slightly louder mix, more bass presence for immersion
*   **Other Cars:** Distance-attenuated, doppler effect applied for passing cars
*   **Volume:** Controlled by "Engine Volume" slider in Settings

**Tire Sounds**
*   **Rolling Noise:** Continuous loop, volume tied to speed
*   **Screeching:** Triggered when lateral or longitudinal slip exceeds threshold (hard braking, cornering at limit, collision)
*   **Surface Variation:** Different pitch/texture for asphalt vs curbs (future feature)
*   **Spatial:** 3D positioned at tire contact points
*   **Volume:** Controlled by "Tire Volume" slider in Settings

**Collision/Impact Sounds**
*   **Trigger:** Car-to-car collision or track boundary impact detected by physics
*   **Intensity:** Volume and pitch scaled by collision velocity/force
*   **One-Shot:** Short impact sound with slight reverb tail
*   **Spatial:** 3D positioned at collision contact point

**Wind Noise (Cockpit Camera Only)**
*   **Source:** Subtle wind loop, volume increases with speed
*   **Attenuation:** Only audible in cockpit camera (simulates open window/helmet)
*   **Volume:** Tied to ambient volume slider

#### 8.2. UI Audio

**Menu Navigation**
*   **Hover:** Soft click/whoosh when hovering over buttons
*   **Click:** Distinct button press sound on activation
*   **Back/Cancel:** Different tone from forward actions (lower pitch)
*   **Volume:** Controlled by "UI Volume" slider in Settings

**Notifications**
*   **Lobby Update:** Subtle chime when players join/leave or sessions update
*   **Error/Warning:** Alert tone for connection issues, invalid actions
*   **Race Start Countdown:** Beep sequence (described in HUD section 7.2)

#### 8.3. Ambient Audio

**Track Environment**
*   **Crowd Ambiance:** Distant crowd murmur loop (future feature, tied to track)
*   **Wind/Weather:** Subtle environmental loops for atmosphere (future feature)
*   **Pit Lane:** Mechanical sounds, air tools when near pits (future feature)
*   **Volume:** Controlled by "Ambient Volume" slider in Settings

#### 8.4. Audio Implementation (Unreal Engine)

*   **System:** UE5's MetaSounds or legacy Sound Cues
*   **Attenuation:** Distance-based volume falloff with customizable curves per sound type
*   **Occlusion:** Basic occlusion for other cars (reduced volume when behind track objects) - future feature
*   **Mix States:** Separate mix states for menu (music-forward) vs racing (SFX-forward)
*   **Compression:** Dynamic range compression to prevent clipping during multi-car scenarios
*   **Performance:** Sound cue budget limits (max 32 concurrent 3D sounds, 8 engine loops)

#### 8.5. Music (Future Feature)

*   **Menu Music:** Ambient electronic/instrumental background tracks
*   **Adaptive Music:** Dynamic music layers that respond to race intensity (lead changes, close battles)
*   **User Music:** Option to play custom music from local files or mute music entirely
*   **Volume:** Separate "Music Volume" slider in Settings (future)

---

### 9. Modding & UGC Considerations (Future)

*   **Asset Loading:** Design `FClientCarConfig` and `FClientTrackConfig` to include paths to actual UE assets (Static Meshes, Materials).
*   **Runtime Asset Loading:** Investigate Unreal Engine's `AssetRegistry` and potential custom asset loaders to allow users to drop in custom car models (`.fbx` + textures) or track models into specific folders that the client can load at runtime.
*   **Configurability:** Ensure car/track configuration files (which define physics parameters) are separate from the visual assets, allowing modders to easily tune physics parameters.

