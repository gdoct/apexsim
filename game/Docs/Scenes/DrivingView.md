# Driving View Scene Specification

## Overview

The Driving View is the main racing scene where players actively drive their cars in a 3D environment. It handles real-time car rendering, physics visualization, player interaction, and race state management.

---

## Scene Type

*   **Implementation:** Full 3D Unreal Engine Level
*   **Server Interaction:** Continuous - sends input at 240Hz, receives telemetry updates
*   **Performance Critical:** Requires consistent 60+ FPS for responsive gameplay

---

## Core Actors & Components

### PlayerCar Actor

The player's own vehicle representation.

#### Components

*   **Skeletal Mesh:** Car body with animated wheels and steering wheel
*   **Camera System:** Multiple camera views (see [Camera System](../Systems/CameraSystem.md))
*   **Input Component:** Captures and processes player input
*   **Client-Side Prediction:** Predicts car movement between server updates
*   **Visual Effects System:** Manages particle effects and animations

#### Input Handling

*   Reads local player input via Enhanced Input System (see [Input System](../Systems/InputSystem.md))
*   Processes throttle, brake, steering inputs each frame
*   Packages input into `ClientMessage::PlayerInput` struct
*   Sends to server via `SimNetClient` at 240Hz
*   Includes `server_tick_ack` for latency calculation

#### Client-Side Prediction

**Purpose:** Provide immediate feedback despite network latency

**Process:**
1. **Local Prediction:** Apply input to local car state immediately
2. **Server Reconciliation:** When server state received, compare to predicted state
3. **Correction:** If mismatch detected, smoothly interpolate to authoritative state
4. **Input Buffer:** Maintain history of inputs for server replay

**Benefits:**
*   Instant steering/throttle response
*   Smooth gameplay even with 50-100ms ping
*   Minimal visual "rubber-banding"

#### Visual Effects

*   **Wheel Rotation:** Animated based on speed and steering angle
*   **Steering Wheel Animation:** Rotates to match steering input
*   **Suspension Compression:** Visual spring offset based on car state
*   **Tire Smoke Particles:**
    *   Triggered on hard braking (brake > 0.8)
    *   Triggered on wheel slip (cornering at limit)
    *   Triggered on collision
    *   Smoke intensity scales with slip amount
*   **Damage/Dirt Overlay (Future):** Progressive dirt accumulation and collision damage

---

### OtherCar Actors

Represents all other players' vehicles in the session.

#### Components

*   **Skeletal Mesh:** Car body (same components as PlayerCar)
*   **Interpolation System:** Smooths movement between telemetry updates
*   **Nameplate Widget:** 3D widget displaying player name and position

#### Server-Driven Movement

**Data Source:** `FClientCarState` structs from `Telemetry` packets

**Interpolation:**
1. **Buffering:** Maintain buffer of 2-3 recent states
2. **Interpolation:** Render position between two buffered states
3. **Extrapolation:** If no recent data, predict forward using last velocity
4. **Smoothing:** Apply position/rotation smoothing to reduce jitter

**Why Interpolation:**
*   Server updates arrive at variable intervals (network jitter)
*   Interpolation provides smooth 60+ FPS rendering from 20-60Hz updates
*   Reduces visual stuttering

#### Visual Effects

Same effects as PlayerCar:
*   Wheel rotation based on speed
*   Suspension animation
*   Tire smoke on braking/sliding
*   Damage/dirt overlays (future)

#### Nameplate Widget

*   **Display:** Player name + position ("P1", "P2", etc.)
*   **Position:** Floating above car, always facing camera (billboard)
*   **Visibility:** Fades with distance (fully visible <50m, invisible >100m)
*   **Color Coding:**
    *   Gold for 1st place
    *   Silver for 2nd
    *   Bronze for 3rd
    *   White for others

---

### TrackActor

The 3D racing circuit representation.

#### Components

*   **Static Mesh:** Track surface geometry
*   **Collision Volumes:** Track boundaries for out-of-bounds detection
*   **Start/Finish Line:** Visual marker and timing gate
*   **Trackside Objects:** Barriers, grandstands, scenery (future)

#### Track Loading

**Process:**
1. `GameMode` receives `FTrackConfigId` from session data
2. Looks up corresponding track asset from `FClientTrackConfig`
3. Loads track level or spawns track actor
4. Initializes track boundaries and timing sectors

#### Track Features

*   **Surface Textures:** Asphalt, curbs, grass, gravel (different friction - future)
*   **Start/Finish Line:** Checkered pattern visual + timing trigger volume
*   **Sector Lines:** Invisible trigger volumes for sector timing (future)
*   **Pit Lane (Future):** Separate pit entry/exit with speed limit zones
*   **Track Boundaries:** Collision volumes to prevent off-track excursions

#### Visual Quality

*   **Texture Resolution:** 2K-4K based on graphics settings
*   **LOD System:** Distance-based level-of-detail for performance
*   **Lighting:** Baked lightmaps for static scenery, dynamic for cars
*   **Shadows:** Cascaded shadow maps for track, dynamic car shadows

---

## HUD Integration

The Driving View scene displays the racing HUD overlay. See [HUD System](../Systems/HUD.md) for complete specifications.

**Core HUD Elements:**
*   Speedometer (bottom-left)
*   Lap counter (top-right)
*   Lap timer (top-right)
*   Position indicator (top-center)
*   Network stats (top-left, optional)

**Conditional Elements:**
*   Countdown timer (center, during SessionState::Countdown)
*   Race results overlay (full-screen, during SessionState::Finished)
*   Pause menu overlay (mid-race, on pause input)

---

## Race State Management

### Session States

The scene responds to different `ERaceSessionState` values:

#### Lobby State
*   Display "Waiting for players..." overlay
*   Cars visible but stationary at grid positions
*   No input accepted for driving (camera control only)
*   Show player list in overlay

#### Countdown State
*   Display countdown timer: "5... 4... 3... 2... 1... GO!"
*   Play countdown audio (beeps synchronized with visuals)
*   Cars locked in grid positions
*   No throttle accepted (brake input allowed for manual start)
*   Camera: Fixed grid camera or player's selected camera

#### Active State (Racing)
*   Full driving controls enabled
*   HUD displays live race data
*   Telemetry updates continuously
*   Audio: Engine sounds, tire sounds, ambient
*   Camera: Player-controlled camera switching

#### Finished State
*   Disable driving input (can still change camera)
*   Display race results overlay
*   Show final standings, lap times, best laps
*   Buttons: "Return to Lobby", "View Replay" (future)

### Transitions

*   **Lobby → Countdown:** Smooth transition, play "race starting" audio cue
*   **Countdown → Active:** Remove countdown overlay, enable input, play "GO" sound
*   **Active → Finished:** Gradual transition, show race results after 2-3 second delay
*   **Finished → Lobby:** Unload driving view, return to CreateJoinSession scene

---

## Performance Optimization

### Rendering

*   **Car Culling:** Don't render cars >500m away (increase for spectator mode)
*   **LOD System:** Reduce car mesh complexity with distance
*   **Particle Budget:** Limit active particle emitters to 32
*   **Shadow Cascades:** Optimize shadow distance and resolution

### Physics

*   **Physics Tick Rate:** 60Hz locally (independent of render FPS)
*   **Collision Channels:** Separate channels for cars, track, and triggers
*   **Simplified Collision:** Use simpler collision meshes for distant cars

### Networking

*   **Bandwidth Management:** UDP packets ~16 bytes for input, ~64 bytes for telemetry
*   **Update Rate:** 240Hz input send, 20-60Hz telemetry receive (server-dependent)
*   **Compression:** Optional delta compression for telemetry (future)

### Audio

*   **3D Audio Budget:** Max 32 concurrent 3D sounds
*   **Priority System:** Player car engine has highest priority
*   **Distance Attenuation:** Reduce volume and stop distant car sounds

---

## Pause Menu

### Activation

*   Trigger: `IA_PauseMenu` input action (default: ESC key)
*   Only available during `SessionState::Active`
*   Cannot pause during countdown

### Pause Menu Overlay

*   **Background:** Blurred/darkened game view (game continues in background)
*   **Options:**
    *   **Resume:** Close pause menu, return to race
    *   **Settings:** Open settings menu (sub-overlay)
    *   **Leave Session:** Confirmation dialog → Return to lobby
    *   **Quit to Desktop:** Confirmation dialog → Exit application

### Behavior

*   **Local Pause Only:** Pausing does not affect other players
*   **Input Disabled:** No driving input while paused
*   **Camera Free-look (Future):** Allow camera rotation while paused
*   **Telemetry Continues:** Server keeps simulating, client keeps receiving updates

---

## Error Handling & Edge Cases

### Connection Loss

*   **Detection:** No telemetry received for >5 seconds
*   **Action:**
    *   Display "Connection Lost" overlay
    *   Freeze all other cars in last known positions
    *   Player car continues with local prediction (ghost mode)
    *   Attempt reconnection (3 retries)
    *   If reconnection fails: "Return to Main Menu" button

### Packet Loss

*   **Detection:** Missing sequence numbers in telemetry
*   **Mitigation:** Interpolation system handles gaps gracefully
*   **Display:** Show packet loss % in network stats (if enabled)

### Desyncs

*   **Detection:** Large mismatch between predicted and server car state (>5m)
*   **Mitigation:** Hard snap to server position (avoid constant rubber-banding)
*   **Logging:** Log desync events for debugging

### Player Disconnection

*   **Other Player Leaves:** Remove OtherCar actor smoothly (fade-out)
*   **Local Player Kicked:** Show "Disconnected from session" message, return to lobby

---

## Future Enhancements

*   **Replay System:** Record race data for playback
*   **Weather System:** Rain, fog, dynamic lighting
*   **Time of Day:** Dynamic day/night cycle
*   **Trackside Cameras:** Spectator-mode TV cameras
*   **Damage Model:** Visual and mechanical damage from collisions
*   **Pit Stops:** Tire changes, repairs, fuel (for endurance racing)
*   **Multiplayer Ghost Mode:** Drive against recorded laps
*   **VR Support:** Stereoscopic rendering for VR headsets
