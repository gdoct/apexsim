# Frontend Architecture Specification

## Overview

This document defines the core architecture of the ApexSim Unreal Engine frontend application, including high-level component organization, data structures, and network communication protocols.

**Engine:** Unreal Engine 5.3+ (latest stable UE version recommended)

**Target Platforms:**
*   **Primary:** Windows 10/11 (64-bit) - DirectX 12, Vulkan
*   **Secondary:** Linux (Ubuntu 22.04+, Fedora 38+) - Vulkan
*   **Future:** macOS (Metal), Steam Deck (Proton/Vulkan)

**Minimum Requirements:**
*   CPU: 4-core @ 2.5GHz (Intel i5-8400 / AMD Ryzen 5 2600 equivalent)
*   GPU: 4GB VRAM, DX12/Vulkan support (GTX 1060 / RX 580 equivalent)
*   RAM: 8GB
*   Storage: 10GB SSD space
*   Network: Broadband connection (5 Mbps down, 1 Mbps up, <100ms ping)

---

## High-Level Architecture

The frontend is a standard Unreal Engine project with core networking and game state interpretation handled in C++ for performance, while UI and scene management leverage a mix of C++ and Blueprints for rapid iteration.

### Core Components (UE Modules/Plugins/Classes)

*   **`GameInstance` / `GameMode`:** Overall game state management and rules
*   **`PlayerController`:** Handles player input and client-side logic
*   **`SimNetClient` (C++):** Custom networking module to communicate with the Rust server
*   **`PlayerCar` / `OtherCar` (C++ / Blueprint):** Base classes for car rendering and visual logic
*   **`TrackActor` (C++ / Blueprint):** Base class for track rendering
*   **UI (UMG):** User interfaces for menus, HUD, and content management

---

## Core Data Structures (C++ Classes / Blueprints)

The client mirrors relevant server data structures, adapting them for UE's environment. These are implemented as USTRUCTs to leverage UE's reflection system and Blueprint accessibility.

### Identifiers

```cpp
// Using FGuid for UE's UUID equivalent
typedef FGuid FPlayerId;
typedef FGuid FSessionId;
typedef FGuid FCarConfigId;
typedef FGuid FTrackConfigId;
```

### Player Data

```cpp
// Client's view of a player
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
```

### Car Configuration

```cpp
// Client's view of a car model
// Used for displaying car names in menus and loading correct visual models
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
```

### Track Configuration

```cpp
// Client's view of a track
// Used for displaying track names in menus and loading correct visual models
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
```

### Car Dynamics State

```cpp
// Received from Server Telemetry - core data used to render cars
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
```

### Race Session State

```cpp
// Client's view of a session
USTRUCT(BlueprintType)
struct FClientRaceSession
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FSessionId Id;
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FTrackConfigId TrackConfigId;
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TMap<FPlayerId, FClientPlayer> ConnectedPlayers; // Mapping player ID to basic player info
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    ERaceSessionState State; // Enum matching server's SessionState
    // Add max_players, lap_limit, etc.
};
```

---

## Network Module (`SimNetClient` C++ Class)

This custom C++ class (likely a `UObject` or a `GameInstance` subclass component) handles all communication with the Rust backend server.

### Responsibilities

#### Initialization
*   Connects UDP/TCP sockets to the server
*   Manages connection state and reconnection logic

#### Serialization/Deserialization
*   Converts UE data structures to/from the Rust server's message formats
*   Uses `bincode` or `flatbuffers` in C++ for performance
*   Ensures cross-platform compatibility (endianness, struct packing)

#### Sending Input
*   Packages `PlayerInput` data (throttle, brake, steering)
*   Sends via UDP at high frequency (240Hz)
*   Minimal packet size for bandwidth efficiency

#### Receiving Telemetry
*   Listens for UDP `Telemetry` packets
*   Deserializes `Telemetry` data into `FClientCarState` structs
*   Manages client-side prediction and interpolation for smooth rendering
*   Publishes events (e.g., "OnTelemetryReceived") or updates game state

#### Receiving Lobby/Session Updates
*   Listens for TCP `ServerMessage`s (LobbyUpdate, SessionCreated, etc.)
*   Updates client's internal representation of available players, sessions, cars, and tracks
*   Publishes events (e.g., "OnLobbyUpdated") for UI elements to react to

### Network Protocol

*   **TCP:** Used for reliable messaging (lobby updates, session management, content lists)
    *   Default port: 9000
    *   Message-based protocol with length prefixes
*   **UDP:** Used for high-frequency, low-latency telemetry and input
    *   Default port: 9001
    *   Packet-based protocol with sequence numbers
    *   Tolerates packet loss (interpolation handles gaps)

### Client-Side Prediction

For the local player's car:
*   Predicts movement locally between server updates
*   Reconciles predicted state with authoritative server state on update
*   Interpolates smoothly to correct any drift
*   Maintains input buffer for server-side reconciliation

### Interpolation

For other players' cars:
*   Buffers received states (typically 2-3 updates)
*   Interpolates position/rotation between buffered states
*   Provides smooth visual movement despite network jitter
*   Handles out-of-order packets gracefully

---

## Modding & UGC Considerations (Future)

### Asset Loading
*   Design `FClientCarConfig` and `FClientTrackConfig` to include paths to actual UE assets (Static Meshes, Materials)
*   Separate visual assets from physics configuration

### Runtime Asset Loading
*   Investigate Unreal Engine's `AssetRegistry` for dynamic loading
*   Design custom asset loaders to allow users to drop in custom content
*   Support formats: `.fbx` + textures for models, `.json` for configuration

### Configurability
*   Ensure car/track configuration files (physics parameters) are separate from visual assets
*   Allow modders to easily tune physics parameters without rebuilding
*   Provide clear documentation for modding API
