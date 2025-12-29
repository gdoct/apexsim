# Camera System Specification

## Overview

The Camera System provides multiple viewpoints for the player during racing, each optimized for different driving styles and preferences. Cameras are switchable in real-time via the `IA_ChangeCamera` input action.

---

## Camera Architecture

### Camera Management

*   **Camera Manager:** `PlayerCameraManager` component in `PlayerController`
*   **Active Camera:** One camera active at a time
*   **Switching:** Cycle through available cameras in order
*   **Persistence:** Last-used camera preference saved to user settings

### Camera Base Class

All cameras inherit from a common base class with shared functionality:

```cpp
class ACameraBase : public AActor
{
    UPROPERTY()
    UCameraComponent* CameraComponent;

    UPROPERTY()
    float FOV; // Field of View in degrees

    virtual void UpdateCamera(float DeltaTime);
    virtual void OnActivated();
    virtual void OnDeactivated();
};
```

---

## Camera Types

### 1. Chase Camera (Default)

**Overview:** Third-person camera following behind the car.

#### Position
*   **Distance Behind Car:** 4-6 meters (scales with speed)
*   **Height Above Ground:** 1.5-2 meters
*   **Offset:** Slight upward angle to see over car

#### Behavior

**Following:**
*   Uses UE's `SpringArmComponent` for smooth following
*   **Lag:** 0.3 seconds (prevents jarring movements)
*   **Rotation Lag:** 0.2 seconds (smooth rotation transitions)

**Speed-Based Distance:**
*   **Low Speed (<50 km/h):** 4 meters behind
*   **Medium Speed (50-150 km/h):** 5 meters behind
*   **High Speed (>150 km/h):** 6 meters behind
*   **Interpolation:** Smooth lerp between distances (2 second transition)

**Look-At Target:**
*   Looks at car's center of mass with 1-meter forward offset
*   Provides view of upcoming track

**Auto-Leveling:**
*   Pitch smoothly levels to horizon (prevents excessive tilt on hills)
*   **Leveling Speed:** 0.5 seconds to level
*   Maintains car's yaw for directional awareness

**Collision Handling:**
*   `SpringArmComponent` retracts if obstructed by track geometry
*   Smoothly extends back to default distance when clear
*   **Probe Channel:** Camera collision channel (ignores cars, detects track only)

#### Settings
*   **FOV:** User-configurable (default: 90 degrees)
*   **Camera Shake:** Respects global shake multiplier

---

### 2. Cockpit Camera (First-Person)

**Overview:** Interior view from driver's perspective.

#### Position
*   **Eye Position:** Defined per car model (driver's head position)
*   **Typically:** ~1.1-1.3 meters above car origin, slightly forward of center

#### Behavior

**Fixed to Car:**
*   1:1 rotation with car (no smoothing/lag)
*   Provides full immersion and precise feedback

**Head Bobbing:**
*   Subtle vertical oscillation tied to suspension movement
*   **Amplitude:** User-configurable (0-100%, default: 50%)
*   **Frequency:** Derived from car's speed and suspension compression

**Look-to-Apex (Optional):**
*   Slight camera yaw toward corner apex during turn-in
*   **Angle:** Max 5-10 degrees toward inside of turn
*   **Trigger:** Steering input >50% + lateral G >0.5
*   **User Setting:** Enable/disable in gameplay settings

**Interior Visibility:**
*   Visible steering wheel (animated to match steering input)
*   Dashboard with functional gauges:
    *   Speedometer (analog or digital)
    *   Tachometer (RPM gauge)
    *   Gear indicator (currently shows "N" for neutral, future: 1-6)
*   Optional: Rearview mirror (renders rear view, performance impact)

#### Settings
*   **FOV:** User-configurable (default: 75 degrees for narrower interior view)
*   **Camera Shake:** Higher sensitivity than chase camera for immersion
*   **Dashboard Visibility:** Toggle to hide dashboard for cleaner view (future)

---

### 3. Hood/Bonnet Camera

**Overview:** Front-mounted camera for balance between visibility and immersion.

#### Position
*   **Location:** Front of car, just above hood
*   **Height:** ~0.5-0.8 meters above car origin
*   **Centered:** On car's centerline

#### Behavior

**Fixed to Car:**
*   Minimal smoothing (0.05s lag for very slight smoothing)
*   Provides clear forward view without interior obstructions

**Camera Shake:**
*   Tied to suspension movement
*   **Amplitude:** Medium (less than cockpit, more than chase)
*   **Sources:** Bumps, kerbs, road texture

**No Interior:**
*   Clean view of track ahead
*   No steering wheel or dashboard visible

#### Settings
*   **FOV:** User-configurable (default: 85 degrees)
*   **Camera Shake:** Respects global shake multiplier

---

### 4. Rear-View Camera (Look Back)

**Overview:** Temporary camera activated by `IA_LookBack` input.

#### Activation
*   **Trigger:** Hold or toggle `IA_LookBack` (default: C key, Right Stick Click)
*   **Mode:**
    *   **Hold Mode (Default):** Active while button held, returns on release
    *   **Toggle Mode (Setting):** Toggles on press, toggles off on second press

#### Position
*   **Location:** Rear of car, looking backward
*   **Distance:** 0.5 meters behind car (closer than chase camera)
*   **Height:** Similar to chase camera height

#### Behavior

**Temporary Override:**
*   Overrides current active camera while active
*   Returns to previous camera on deactivation

**Field of View:**
*   Slightly narrower than normal cameras (simulates focusing on rear)
*   **FOV:** Base FOV - 10 degrees (e.g., 90° → 80°)

**Camera Movement:**
*   Minimal lag (0.1s) for stability

#### Use Cases
*   Check for cars behind
*   Defensive driving (blocking positions)
*   Situational awareness

---

### 5. TV Camera / Spectator Mode (Future)

**Overview:** Trackside static cameras for replay and spectator viewing.

#### Position
*   **Static Locations:** Predefined camera positions at key track locations:
    *   Start/finish straight
    *   Major corners
    *   Overtaking zones
    *   Scenic viewpoints

#### Behavior

**Automatic Switching:**
*   Switches to nearest/best-angle camera as player passes
*   **Trigger Distance:** Activate when car within 50-100m of camera
*   **Transition:** Cut or slow pan (user-configurable)

**Camera Tracking:**
*   Camera rotates to follow passing cars
*   Smooth pan to keep car in frame
*   **Pan Speed:** 30-60 degrees per second

**Multiple Car Tracking:**
*   If multiple cars in view, tracks race leader or closest car

#### Use Cases
*   **Replay Viewing:** Cinematic camera angles for reviewing races
*   **Spectator Mode:** Watch other players race
*   **Highlights:** Auto-generated highlight reels

---

## Camera Smoothing & Responsiveness

### Spring Arm Lag

Controls how quickly camera follows car's position:

*   **Chase Camera:** 0.3s (smooth, cinematic)
*   **Cockpit Camera:** 0.0s (instant, realistic)
*   **Hood Camera:** 0.1s (slight smoothing)
*   **Rear-View Camera:** 0.1s (stable rear view)

### Rotation Lag

Separate lag for camera rotation (prevents jarring movements):

*   **Chase Camera:** 0.2s (smooth yaw transitions)
*   **Cockpit Camera:** 0.0s (instant, full car feedback)
*   **Hood Camera:** 0.05s (very slight smoothing)

### Speed-Based FOV (Optional)

Dynamic FOV adjustment simulating tunnel vision at high speeds:

*   **Enabled By:** User setting (default: off)
*   **Effect:** Gradually increase FOV as speed increases
*   **Max Increase:** +10 degrees at 250+ km/h
*   **Example:** 90° FOV → 100° FOV at top speed
*   **Interpolation:** Smooth lerp over 2-3 seconds

---

## Camera Shake

Adds realism through procedural shake effects.

### Shake Sources

1. **Suspension Travel**
    *   Triggered by bumps, kerbs, track irregularities
    *   Intensity proportional to suspension compression rate
    *   Frequency: 5-15 Hz (varies with road texture)

2. **Engine Vibration**
    *   Subtle high-frequency shake tied to RPM
    *   **RPM-Based Frequency:** 20-60 Hz (higher at high RPM)
    *   **Amplitude:** Very low (barely noticeable, adds immersion)

3. **Collisions**
    *   Impulse-based shake on car-to-car or wall impacts
    *   **Intensity:** Proportional to collision force
    *   **Duration:** 0.2-0.5 seconds (decays over time)
    *   **Type:** Sharp spike followed by decay

### User Control

*   **Global Shake Multiplier:** 0-100% in Settings > Gameplay > Camera Shake
*   **Per-Camera Intensity:**
    *   Cockpit: 100% of base intensity (most shake)
    *   Hood: 75% of base intensity
    *   Chase: 40% of base intensity (least shake, more cinematic)

### Implementation

Uses UE's `CameraShakeBase` system:

```cpp
class UCarCameraShake : public UCameraShakeBase
{
    UPROPERTY()
    FVector LocOffset; // Position shake

    UPROPERTY()
    FRotator RotOffset; // Rotation shake

    UPROPERTY()
    float Frequency;    // Oscillation frequency

    UPROPERTY()
    float Amplitude;    // Shake intensity
};
```

*   **Procedural Patterns:** Perlin noise for organic feel
*   **Frequency Variation:** Avoid repetitive patterns
*   **Damping:** Gradual decay for natural feel

---

## Camera Collision Detection

### Chase Camera Collision

Uses `SpringArmComponent` built-in collision:

*   **Probe Size:** Small sphere (radius: 12 units)
*   **Collision Channel:** Custom "CameraTrace" channel
    *   Blocks: Track geometry, static meshes
    *   Ignores: Cars, triggers, particles
*   **Behavior:**
    *   If probe hits track, retract spring arm to hit point
    *   Smoothly extend back when clear (0.5s transition)

### Other Camera Collision

*   **Cockpit/Hood:** No collision detection (cameras inside car)
*   **Rear-View:** Same as chase camera (spring arm retraction)
*   **TV Camera (Future):** Static positions, no collision detection needed

---

## Camera Transitions

### Switching Cameras

*   **Input:** `IA_ChangeCamera` cycles through available cameras
*   **Cycle Order:**
    1. Chase Camera
    2. Cockpit Camera
    3. Hood Camera
    4. (TV Camera - future)
*   **Transition Type:**
    *   **Instant Cut (Default):** No blend, immediate switch
    *   **Blend (Optional Setting):** 0.2s blend between cameras

### Visual Feedback

*   **On-Screen Indicator:** Briefly display camera name on switch
    *   "Chase Camera", "Cockpit", "Hood", etc.
    *   **Duration:** 1.5 seconds fade-out
    *   **Position:** Bottom-center of screen

---

## Accessibility & Customization

### User Settings

All camera settings accessible in Settings > Gameplay:

*   **Default Camera:** Choose starting camera (Chase, Cockpit, Hood)
*   **Camera FOV:** Global FOV slider (60-120°)
*   **Camera Shake:** Global shake intensity (0-100%)
*   **Speed-Based FOV:** Enable/disable dynamic FOV
*   **Look-to-Apex:** Enable/disable cockpit look-to-apex

### Per-Camera FOV Override (Future)

*   Allow separate FOV settings per camera type
*   Useful for users who prefer different FOVs for different views

### Motion Sickness Mitigation

*   **Reduced Shake Option:** Lower default shake values
*   **Fixed Horizon:** More aggressive auto-leveling for chase camera
*   **Cockpit Stability:** Option to reduce head bobbing amplitude

---

## Performance Considerations

### Rendering

*   **Single Active Camera:** Only render from one camera at a time (except rearview mirror)
*   **Rearview Mirror (Future):** Render-to-texture at lower resolution (256x128)
    *   **Performance Impact:** ~5-10% FPS reduction
    *   **User Toggle:** Enable/disable in graphics settings

### Camera Updates

*   **Update Frequency:** Every frame (tied to render loop)
*   **CPU Cost:** Minimal (<0.1ms per frame for camera calculations)
*   **Spring Arm Traces:** ~0.05ms per trace (only chase/rear-view cameras)

### LOD Interaction

*   Cameras can affect LOD calculations
*   Objects closer to camera rendered at higher detail
*   Chase camera sees more of car, so car uses higher LOD than cockpit view

---

## Future Enhancements

*   **Custom Camera Positions:** User-defined camera positions via in-game editor
*   **Replay Camera Director:** AI-powered camera selection for replays (finds best angles)
*   **Photo Mode:** Freeze time, free-camera for screenshots
*   **VR Support:** Stereoscopic rendering, head tracking for cockpit camera
*   **Drone Camera:** Free-flying camera for spectator mode
*   **Picture-in-Picture:** Small rear-view display while using other cameras
