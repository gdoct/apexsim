# Audio System Specification

## Overview

The Audio System provides immersive sound feedback for racing, UI interactions, and environmental ambiance. It uses Unreal Engine 5's audio capabilities to deliver realistic 3D positional audio and dynamic soundscapes.

---

## Audio Architecture

### Audio Engine

*   **System:** UE5's MetaSounds (preferred) or legacy Sound Cues
*   **Spatial Audio:** 3D positioned sounds with distance attenuation
*   **Mix System:** Dynamic audio mixing with separate volume controls
*   **Format:** Compressed audio (OGG Vorbis) for efficiency

### Audio Categories

Four main categories with independent volume controls:

1. **Car Audio:** Engine, tires, collisions
2. **UI Audio:** Menu sounds, notifications
3. **Ambient Audio:** Environment, crowd, weather
4. **Music Audio (Future):** Background music, adaptive tracks

---

## Car Audio

### Engine Sounds

**Multi-Layered System:**

*   **Layer Structure:**
    *   Idle layer (0-1000 RPM)
    *   Low layer (1000-3000 RPM)
    *   Mid layer (3000-5000 RPM)
    *   High layer (5000-7000 RPM)
    *   Redline layer (7000+ RPM)

*   **Crossfading:** Smooth blending between layers based on RPM
    *   **Blend Range:** ±500 RPM around transition points
    *   **Interpolation:** Linear crossfade over blend range

**Playback System:**

*   **Real-Time Pitch Shifting:**
    *   Base samples recorded at fixed RPM (e.g., 3000 RPM per layer)
    *   Pitch shifted in real-time based on current RPM
    *   **Formula:** `Pitch = CurrentRPM / BaseSampleRPM`
    *   **Range:** 0.7x - 1.5x pitch (prevents unnatural sounds)

*   **RPM Calculation:**
    *   Derived from speed and gear (server provides speed, gear inferred or sent)
    *   **Simplified:** `RPM = (Speed * 60 * GearRatio) / WheelCircumference`
    *   **Placeholder:** Use speed-based approximation until gear data available

**Spatial Positioning:**

*   **3D Position:** Attached to car's engine location (front for front-engine, rear for mid/rear-engine)
*   **Attenuation:** Distance-based volume falloff
    *   **Falloff Start:** 5 meters
    *   **Falloff End:** 100 meters
    *   **Curve:** Logarithmic (natural sound propagation)

**Player vs Other Cars:**

*   **Player Car:**
    *   Slightly louder mix (+3 dB)
    *   More bass presence for immersion
    *   Closer listener position (cockpit camera affects this)
*   **Other Cars:**
    *   Standard volume
    *   Doppler effect applied for passing cars
    *   Distance attenuation based on listener position

**Volume Control:**

*   User setting: "Engine Volume" (0-100%, default 75%)
*   Affects all engine sounds proportionally

---

### Tire Sounds

**Rolling Noise:**

*   **Type:** Continuous loop
*   **Volume:** Tied to speed
    *   **Formula:** `Volume = min(Speed / MaxSpeed, 1.0)`
    *   Silent at standstill, full volume at high speed
*   **Pitch:** Slightly increases with speed (simulates frequency change)
*   **Spatial:** 3D positioned at tire contact points (or car center for simplicity)

**Screeching/Sliding:**

*   **Trigger Conditions:**
    *   Hard braking (brake input >80%)
    *   Lateral slip (cornering at limit, high lateral G)
    *   Wheel lock (future: when slip ratio >100%)
    *   Collision with track boundaries
*   **Volume:** Proportional to slip amount
    *   **Formula:** `Volume = SlipIntensity * TireVolumeSetting`
*   **Pitch:** Varies with slip speed (faster slip = higher pitch)
*   **Spatial:** 3D positioned at individual tires (or car center)

**Surface Variation (Future):**

*   Different tire sounds for different surfaces:
    *   **Asphalt:** Standard road noise
    *   **Curbs:** Harsher, rumbling sound
    *   **Grass/Gravel:** Softer, muffled sound
*   Requires track surface data from server or track model

**Volume Control:**

*   User setting: "Tire Volume" (0-100%, default 75%)
*   Affects rolling noise and screeching

---

### Collision/Impact Sounds

**Trigger:**

*   Car-to-car collision detected by physics
*   Track boundary/wall impact
*   Collision force threshold >100N (prevents sound spam from minor touches)

**Sound Selection:**

*   **Light Impact (<500N):** Short, dull thud
*   **Medium Impact (500-2000N):** Metallic crunch
*   **Heavy Impact (>2000N):** Loud crash with debris sounds

**Intensity Scaling:**

*   **Volume:** Proportional to collision force
    *   **Formula:** `Volume = min(Force / MaxForce, 1.0)`
*   **Pitch:** Lower pitch for heavier impacts (more bass)

**Spatial Positioning:**

*   3D positioned at collision contact point
*   Uses UE's physics collision data for accurate positioning

**Reverb:**

*   Short reverb tail (0.2-0.5s) for environment feel
*   Simulates sound bouncing off nearby track objects

**Volume Control:**

*   Inherits from "Engine Volume" setting (car-related sounds)

---

### Wind Noise (Cockpit Camera Only)

**Purpose:** Enhance immersion in cockpit view

*   **Type:** Subtle wind loop
*   **Volume:** Increases with speed
    *   **Formula:** `Volume = (Speed / MaxSpeed) * 0.5`
    *   Max 50% volume even at top speed (prevents overpowering other sounds)
*   **Attenuation:** Only audible in cockpit camera view
    *   Silent in chase, hood, or other external cameras
*   **Pitch:** Slight pitch variation with speed

**Volume Control:**

*   Tied to "Ambient Volume" slider

---

## UI Audio

### Menu Navigation Sounds

**Hover Sound:**

*   **Trigger:** Mouse hover or focus change on buttons/menu items
*   **Sound:** Soft click or subtle whoosh (20-50ms duration)
*   **Volume:** Quiet (50% of max UI volume)

**Click/Select Sound:**

*   **Trigger:** Button press, menu selection confirmed
*   **Sound:** Distinct button press sound (50-100ms duration)
*   **Pitch:** Consistent, clear tone
*   **Volume:** Standard UI volume

**Back/Cancel Sound:**

*   **Trigger:** Back button, cancel action, close dialog
*   **Sound:** Different tone from forward actions (lower pitch)
*   **Purpose:** Audio distinction between forward/backward navigation
*   **Volume:** Standard UI volume

**Volume Control:**

*   User setting: "UI Volume" (0-100%, default 75%)

---

### Notifications

**Lobby Update Sound:**

*   **Trigger:** Player joins/leaves lobby, session created/destroyed
*   **Sound:** Subtle chime (200ms duration)
*   **Volume:** Medium (75% of max UI volume)
*   **Prevent Spam:** Max 1 sound per second (batch multiple updates)

**Error/Warning Sound:**

*   **Trigger:** Connection lost, invalid action, error message
*   **Sound:** Alert tone (distinctive, slightly harsh)
*   **Volume:** Louder than other UI sounds (100% of max UI volume)
*   **Urgency:** Two-tone beep for critical errors

**Race Start Countdown Sounds:**

*   **Countdown Beeps (5-2):**
    *   Low pitch beep (500 Hz)
    *   Duration: 100ms
    *   Synchronized with visual countdown
*   **Countdown Beep (1):**
    *   Higher pitch beep (800 Hz)
    *   Duration: 100ms
*   **GO Sound:**
    *   Distinctive race start sound (air horn or horn blast)
    *   Duration: 300ms
    *   Louder volume for impact

**Volume Control:**

*   Tied to "UI Volume" slider

---

## Ambient Audio

### Track Environment

**Crowd Ambiance (Future):**

*   **Type:** Distant crowd murmur loop
*   **Variation:** Cheers on overtakes, race start, finish
*   **Spatial:** Positioned at grandstand locations on track
*   **Volume:** Subtle background layer (30-50% of ambient volume)

**Wind/Weather (Future):**

*   **Wind Loop:** Subtle environmental wind for outdoor tracks
*   **Rain Sounds:** Rain on car, rain ambiance for weather conditions
*   **Dynamic:** Intensity varies with weather system

**Pit Lane (Future):**

*   **Mechanical Sounds:** Air tools, engine revs, crew radio chatter
*   **Trigger:** When player car near pit lane
*   **Spatial:** 3D positioned in pit area

**Volume Control:**

*   User setting: "Ambient Volume" (0-100%, default 75%)
*   Affects all environmental sounds

---

## Audio Implementation (Unreal Engine)

### MetaSounds vs Sound Cues

**MetaSounds (Preferred):**

*   UE5's modern procedural audio system
*   Real-time parameter control (RPM, speed, slip)
*   Modular graph-based design
*   Better performance for dynamic sounds

**Sound Cues (Fallback):**

*   Legacy system, widely supported
*   Simpler for basic sounds (UI, impacts)
*   Less flexible for real-time parameter changes

### Attenuation Settings

**Distance-Based Volume Falloff:**

*   **Attenuation Shape:** Sphere
*   **Falloff Start Distance:** Varies by sound type
    *   Engine: 5m
    *   Tires: 3m
    *   Collisions: 2m
*   **Falloff End Distance:**
    *   Engine: 100m
    *   Tires: 50m
    *   Collisions: 80m
*   **Curve:** Logarithmic (Natural_Sound attenuation)

### Occlusion (Future)

*   **Basic Occlusion:** Reduce volume of sounds behind track objects
*   **Ray Casting:** Trace from listener to sound source
*   **Attenuation:** -6 to -12 dB if occluded
*   **Performance:** Limit to 16 concurrent occlusion traces

### Mix States

**Menu Mix State:**

*   Music: 100% (future)
*   UI: 100%
*   Car: 0% (muted)
*   Ambient: 50%

**Racing Mix State:**

*   Music: 0% (or 30% if enabled)
*   UI: 75%
*   Car: 100%
*   Ambient: 75%

**Pause Mix State:**

*   Music: 0%
*   UI: 100%
*   Car: 20% (ducked, background)
*   Ambient: 20% (ducked)

### Dynamic Range Compression

**Purpose:** Prevent audio clipping during multi-car scenarios

*   **Compressor Settings:**
    *   Threshold: -6 dB
    *   Ratio: 4:1
    *   Attack: 10ms
    *   Release: 100ms
*   **Effect:** Reduces peaks when many sounds play simultaneously
*   **Target:** Maintain clear audio even with 20 cars on track

### Audio Budget

**Performance Limits:**

*   **Max 3D Sounds:** 32 concurrent
*   **Max 2D Sounds (UI):** 8 concurrent
*   **Max Engine Loops:** 20 (one per car)
*   **Priority System:**
    1. Player car engine (highest)
    2. Nearby cars engines (distance-based)
    3. Collisions
    4. Tires
    5. Ambient (lowest)

**Voice Stealing:**

*   If budget exceeded, lowest priority sounds are culled
*   Sounds beyond falloff distance culled first
*   Prevents audio glitches from too many simultaneous sounds

---

## Music System (Future)

### Menu Music

*   **Type:** Ambient electronic/instrumental background tracks
*   **Loop:** Seamless looping for indefinite playback
*   **Volume:** Separate "Music Volume" slider (0-100%, default 50%)
*   **Ducking:** Reduce volume during UI interactions (voice-over priority)

### Adaptive Racing Music

*   **Dynamic Layers:** Music intensity adapts to race events
    *   Base layer: Ambient track during normal racing
    *   Intensity layer 1: Added during close battles
    *   Intensity layer 2: Added during final lap
    *   Crescendo: Full orchestration on race finish
*   **Triggers:**
    *   Close proximity to other cars (<5m)
    *   Position changes (overtake/overtaken)
    *   Final lap
    *   Finish line crossing

### User Music Support

*   **Custom Music:** Option to play music from local files (MP3, OGG, WAV)
*   **Playlist:** Create playlists in settings
*   **Mute Option:** Disable all music entirely
*   **Integration:** Uses UE's media framework for playback

---

## Accessibility & Customization

### Volume Mixing

Four independent volume controls:

*   **Master Volume:** Global volume (0-100%, default 75%)
*   **Engine Volume:** Car sounds (0-100%, default 75%)
*   **Tire Volume:** Tire sounds (0-100%, default 75%)
*   **Ambient Volume:** Environment (0-100%, default 75%)
*   **UI Volume:** Menus, notifications (0-100%, default 75%)

**Master Volume Override:**

*   All category volumes multiplied by master volume
*   **Formula:** `FinalVolume = CategoryVolume * MasterVolume`

### Audio Output Device

*   **Device Selection:** Dropdown in Settings > Audio
*   **Options:** System default + all detected audio devices
*   **Hot-Swap:** Detect device changes, update available devices list
*   **Auto-Switch:** Prompt user if current device disconnected

### Mono/Stereo Options (Future)

*   Option to force mono output (for single speaker setups)
*   Stereo width adjustment (0-200%, for headphone users)

### Audio Subtitles (Future)

*   Visual indicators for audio events (for hearing-impaired users)
*   Examples: "[Engine revving]", "[Tire screeching]", "[Collision]"
*   Positioned near sound source on-screen

---

## Performance Considerations

### Audio Thread

*   UE5 uses separate audio thread for mixing
*   Minimal impact on game thread (<0.5ms per frame)
*   Offloads audio processing from main rendering loop

### Streaming

*   Long sounds (music, ambient loops) streamed from disk
*   Short sounds (UI, impacts) loaded into memory
*   Reduces memory footprint

### Compression

*   All sounds compressed with OGG Vorbis
*   Quality: 192 kbps for music, 128 kbps for SFX, 96 kbps for ambient
*   Balance: Quality vs file size

### Memory Budget

*   **Target:** <100 MB for all loaded audio assets
*   **Loaded Sounds:** UI sounds, car engine layers, tire sounds
*   **Streamed Sounds:** Music, ambient loops

---

## Future Enhancements

*   **Replay Commentary:** AI-generated or pre-recorded commentary during replays
*   **Spotter/Engineer Radio:** Voice callouts for race info (gaps, fuel, tires)
*   **Advanced Weather Audio:** Rain intensity variation, thunder
*   **Pit Radio Chatter:** Crew communications during pit stops
*   **Surface-Specific Sounds:** Different sounds for asphalt, concrete, gravel
*   **Transmission Sounds:** Gear shift sounds (whine, clunk)
*   **Aerodynamic Sounds:** Downforce whoosh at high speed
*   **Tire Degradation Audio:** Sound changes as tires wear
*   **3D Audio Support:** Dolby Atmos, DTS:X for advanced spatial audio
