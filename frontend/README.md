
## SimRacing Frontend Application Specification (Unreal Engine) - Initial Phase

**Project Goal:** Develop a responsive Unreal Engine client application capable of connecting to the Rust backend server, displaying game menus, rendering a basic driving view based on server telemetry, and sending player input. Designed for cross-platform compatibility and future extensibility.

**Engine:** Unreal Engine 5 (or latest stable UE version)
**Target Platform:** Initial development on Linux, targeting any Unreal Engine compatible platform (Windows, macOS, etc.) for eventual distribution.

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
    *   Placeholder options for graphics, audio, controls.
    *   **"Save" / "Apply" / "Back" Buttons.**
*   **Logic:** Basic UI interaction, saving settings to local config file. No server interaction.

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
        *   **Camera:** Attached to `PlayerCar` (e.g., chase cam).
        *   **Input Component:** Reads local player input (steering, throttle, brake) and sends it via `SimNetClient`.
        *   **Client-side Prediction/Interpolation:** Crucial for smoothness. The `PlayerCar` will predict its movement locally between server updates. When a server update arrives, it will reconcile its predicted state with the authoritative server state, interpolating smoothly to correct any drift.
    *   **`OtherCar` Actors:** Spawns basic car meshes for all other players in the session.
        *   These only render the car's visual model. Their movement is purely driven by `Telemetry` received from the server, using interpolation between received `FClientCarState` updates for smoothness.
    *   **`TrackActor`:** Spawns a basic 3D track mesh (initially a flat plane with texture).
    *   **`HUD` Widget (UMG):** Overlays on top of the 3D scene.
        *   Displays `Speed`, `RPM` (placeholder), `Lap Time`, `Best Lap`, `Current Lap` from `FClientCarState`.
*   **Logic:**
    *   `GameMode` loads the specific `TrackActor` based on `Session` data.
    *   `SimNetClient` continuously updates `FClientCarState` data for all cars in the session.
    *   `PlayerCar` and `OtherCar` actors use this `FClientCarState` (and client-side prediction for the local player) to update their 3D positions and rotations.

---

### 5. Input Management

*   **`PlayerController`:** Standard UE `PlayerController` will handle input from keyboard, gamepad, or simulated steering wheel.
*   **Binding:** Map physical inputs to abstract game inputs (throttle 0-1, brake 0-1, steering -1 to 1).
*   **Transmission:** `PlayerController` passes these processed inputs to `SimNetClient` to be sent to the server.

---

### 6. Modding & UGC Considerations (Future)

*   **Asset Loading:** Design `FClientCarConfig` and `FClientTrackConfig` to include paths to actual UE assets (Static Meshes, Materials).
*   **Runtime Asset Loading:** Investigate Unreal Engine's `AssetRegistry` and potential custom asset loaders to allow users to drop in custom car models (`.fbx` + textures) or track models into specific folders that the client can load at runtime.
*   **Configurability:** Ensure car/track configuration files (which define physics parameters) are separate from the visual assets, allowing modders to easily tune physics parameters.

