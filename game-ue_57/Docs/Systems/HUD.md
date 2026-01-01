# HUD (Heads-Up Display) Specification

## Overview

The HUD provides essential racing information during gameplay without obstructing the view. All elements are designed for at-a-glance readability during high-speed racing.

---

## HUD Architecture

### Implementation

*   **Type:** UMG Widget Blueprint
*   **Parent Class:** `UUserWidget`
*   **Attachment:** Overlaid on viewport during Driving View scene
*   **Update Frequency:** Every frame (60+ FPS)

### Data Sources

*   **Telemetry Data:** `FClientCarState` from `SimNetClient`
*   **Session Data:** `FClientRaceSession` state and timing information
*   **Input Data:** Local player's input values (for optional input display)
*   **Network Stats:** Ping, packet loss from `SimNetClient` (if enabled)

---

## Core HUD Elements

### 1. Speedometer (Bottom-Left)

**Display:**
*   Large numeric speed value (e.g., "156")
*   Unit label (km/h, mph, or m/s based on user setting)
*   Example: "156 km/h"

**Style:**
*   **Font:** Bold sans-serif (readable at high speed)
*   **Size:** Large (48-60pt for number, 24pt for unit)
*   **Color:** White text with dark semi-transparent background (RGBA: 0, 0, 0, 180)
*   **Outline:** Subtle black outline for contrast against light backgrounds

**Data Source:**
*   `FClientCarState.SpeedMps` converted to user's preferred unit:
    *   km/h: `SpeedMps * 3.6`
    *   mph: `SpeedMps * 2.237`
    *   m/s: `SpeedMps` (no conversion)

**Position:**
*   Bottom-left corner, 5% margin from edges
*   Anchor: Bottom-left

---

### 2. Tachometer (Bottom-Left, adjacent to speedometer)

**Current Implementation (Placeholder):**
*   Text: "RPM: ----"
*   Same style as speedometer
*   Position: Below or to the right of speedometer

**Future Implementation:**
*   Arc-style RPM gauge (0-8000 RPM typical range)
*   Color-coded zones:
    *   Green: Optimal power band
    *   Yellow: Approaching redline (7000-7500 RPM)
    *   Red: Redline zone (7500-8000+ RPM)
*   Numeric RPM value in center

**Additional Info:**
*   **Gear Indicator:** Small numeric or letter display
    *   Current: Always shows "N" (neutral, automatic transmission)
    *   Future: Shows current gear (1-6, R for reverse)

---

### 3. Lap Counter (Top-Right)

**Display:**
*   "Lap X / Y" format
    *   X = Current lap number
    *   Y = Total lap limit from session config
*   Example: "Lap 3 / 5"

**Style:**
*   **Font:** Bold sans-serif
*   **Size:** Medium (28-32pt)
*   **Color:** White text, dark semi-transparent background
*   **Highlight:** Changes color on final lap
    *   Normal: White
    *   Final Lap: Yellow or orange

**Data Source:**
*   Current lap from telemetry or session state
*   Lap limit from `FClientRaceSession.lap_limit`

**Position:**
*   Top-right corner, 5% margin from edges
*   Anchor: Top-right

---

### 4. Lap Timer (Top-Right, below lap counter)

**Display Components:**

**Current Lap Time:**
*   Live timer showing ongoing lap time
*   Format: MM:SS.mmm (minutes:seconds.milliseconds)
*   Example: "01:45.234"
*   Updates every frame for smooth counting

**Best Lap Time:**
*   Displayed below current time with "Best: " prefix
*   Example: "Best: 01:43.891"
*   Color: Green if current lap is faster, white otherwise

**Delta Indicator (Future):**
*   Shows +/- time difference from best lap
*   Example: "+0.345" (behind) or "-0.123" (ahead)
*   Color: Green if ahead, red if behind

**Style:**
*   **Font:** Monospaced (for stable digit alignment)
*   **Size:** 24-28pt for current time, 20-24pt for best time
*   **Color:** Current time white, best time green (if beating) or white
*   **Background:** Semi-transparent dark panel

**Data Source:**
*   Timing data from server telemetry or session state
*   Local timer synchronized with server tick

**Position:**
*   Below lap counter, aligned to right
*   Anchor: Top-right

---

### 5. Position Indicator (Top-Center)

**Display:**
*   Ordinal position in race
*   Format: "P1", "P2", etc. or "1st", "2nd", "3rd", "4th+", etc.
*   Example: "1st" or "P3"

**Style:**
*   **Font:** Bold sans-serif
*   **Size:** Large (48-60pt)
*   **Color:** Position-based coloring:
    *   **1st Place:** Gold (#FFD700)
    *   **2nd Place:** Silver (#C0C0C0)
    *   **3rd Place:** Bronze (#CD7F32)
    *   **Other:** White
*   **Background:** Dark semi-transparent panel
*   **Border:** Thick border matching text color

**Data Source:**
*   Race position calculated from track progress of all cars
*   Updated from server session state

**Position:**
*   Top-center of screen
*   Anchor: Top-center

---

### 6. Minimap (Bottom-Right) - Future

**Display:**
*   Simplified top-down track view
*   All car positions shown as colored dots/icons
*   Player car highlighted with distinct color/icon

**Features:**
*   **Zoom Levels:**
    *   Close: Show nearby cars (50m radius)
    *   Medium: Show section of track (200m radius)
    *   Far: Show entire track
*   **Auto-Zoom:** Automatically adjust to show relevant cars
*   **Rotation:** North-up or heading-up (user preference)

**Style:**
*   Circular or rectangular panel
*   Semi-transparent background
*   Track outline in white, cars as colored dots

**Position:**
*   Bottom-right corner, 5% margin
*   Anchor: Bottom-right

---

### 7. Input Display (Bottom-Center) - Optional

**Display:**
*   Visual bars showing current input values:
    *   **Throttle Bar:** Vertical bar, fills upward (green)
    *   **Brake Bar:** Vertical bar, fills upward (red)
    *   **Steering Bar:** Horizontal bar, center-zero (blue/white)
*   Percentage labels (0-100%)

**Style:**
*   Simple bar graphs with clear labels
*   Color-coded for quick recognition
*   Compact layout to minimize screen space

**Visibility:**
*   **User Setting:** Toggleable in Settings > Gameplay
*   **Default:** Off (hidden)

**Use Cases:**
*   Analyzing driving technique
*   Reviewing replays
*   Learning throttle/brake modulation

**Data Source:**
*   Local player input values from `PlayerController`

**Position:**
*   Bottom-center, above speedometer
*   Anchor: Bottom-center

---

## Conditional HUD Elements

### 8. Countdown Timer (Center-Screen)

**Display:**
*   Large countdown numbers: "5... 4... 3... 2... 1... GO!"
*   Fade-in/fade-out animations for each number
*   "GO!" displayed for 1 second before disappearing

**Style:**
*   **Font:** Very bold, large (120-180pt)
*   **Color:**
    *   Numbers 5-2: White or yellow
    *   Number 1: Orange
    *   "GO!": Green
*   **Animation:**
    *   Scale-in effect (starts large, shrinks slightly)
    *   Fade-out at end of each second
    *   Pulsing effect for dramatic impact

**Audio Sync:**
*   Synchronized beep sounds with each number
    *   5-2: Low pitch beep
    *   1: Higher pitch beep
    *   GO: Distinct "race start" sound

**Visibility:**
*   Only shown during `SessionState::Countdown`
*   Disappears on transition to `SessionState::Active`

**Position:**
*   Dead center of screen
*   Anchor: Center

---

### 9. Network Stats (Top-Left) - Optional

**Display:**
*   **Ping:** "Ping: 45ms"
*   **Packet Loss:** "Loss: 0.5%"
*   **Server Tick (Debug):** "Tick: 240" (server tick rate)

**Style:**
*   Small monospaced font (16-20pt)
*   Color-coded by quality:
    *   **Green:** Ping <50ms, Loss <1%
    *   **Yellow:** Ping 50-100ms, Loss 1-5%
    *   **Red:** Ping >100ms, Loss >5%
*   Semi-transparent background

**Visibility:**
*   **User Setting:** Settings > Network > Show Network Stats
*   **Default:** Off (hidden)

**Data Source:**
*   Network statistics from `SimNetClient`
*   Updated every 0.5 seconds (not every frame)

**Position:**
*   Top-left corner, 5% margin
*   Anchor: Top-left

---

### 10. Race Results Overlay (Post-Race)

**Display:**
*   Full-screen semi-transparent overlay
*   Shows final standings in table format:
    *   Position | Player Name | Total Time | Best Lap

**Example:**
```
         RACE RESULTS
================================
1st   PlayerOne     05:23.456   01:03.123
2nd   PlayerTwo     05:24.891   01:03.456
3rd   PlayerThree   05:27.234   01:04.012
...
```

**Style:**
*   Large title at top
*   Table with clear columns
*   Player's own row highlighted
*   Top 3 positions color-coded (gold, silver, bronze)

**Actions:**
*   **"Return to Lobby" Button:** Exit to CreateJoinSession scene
*   **"View Replay" Button (Future):** Load replay viewer

**Visibility:**
*   Shown when `SessionState::Finished`
*   Appears after 2-3 second delay (allow final moments to be seen)

**Position:**
*   Full-screen overlay
*   Centered content

---

### 11. Pause Menu Overlay (Mid-Race)

**Display:**
*   Semi-transparent full-screen overlay
*   Background: Blurred/darkened game view
*   Menu options in center

**Menu Options:**
*   **Resume:** Close pause menu, return to race
*   **Settings:** Open settings submenu (overlay on top)
*   **Leave Session:** Confirmation dialog → Return to lobby
*   **Quit to Desktop:** Confirmation dialog → Exit application

**Style:**
*   Large, clear buttons
*   Current selection highlighted
*   Keyboard/controller navigation supported

**Visibility:**
*   Shown when `IA_PauseMenu` input triggered
*   Not available during `SessionState::Countdown`

**Position:**
*   Full-screen overlay
*   Centered menu

---

## HUD Customization

All HUD elements respect user settings from Settings > Gameplay:

### HUD Opacity (0-100%)

*   **Effect:** Adjusts transparency of all HUD background panels
*   **Default:** 80%
*   **0%:** Fully transparent backgrounds (text only)
*   **100%:** Fully opaque backgrounds

### HUD Scale (0.5x - 1.5x)

*   **Effect:** Scales all HUD elements proportionally
*   **Default:** 1.0x (100%)
*   **0.5x:** Half size (for high-res displays or minimal HUD)
*   **1.5x:** 50% larger (for accessibility or low-res displays)

### Speedometer Units

*   **Options:** km/h, mph, m/s
*   **Default:** km/h
*   **Effect:** Changes unit displayed and conversion formula

### Element Visibility (Future)

*   Individual toggles for each HUD element
*   Allow players to hide elements they don't need
*   Presets: "Minimal", "Standard", "Full"

### Color Themes (Future)

*   Support for colorblind-friendly palettes
*   Options: Default, Protanopia, Deuteranopia, Tritanopia
*   Changes color coding for position indicators, delta times, etc.

---

## Responsive Layout

### Aspect Ratio Support

HUD adapts to various screen aspect ratios:

*   **16:9 (Standard):** Default layout
*   **21:9 (Ultrawide):** Elements move further to edges, maintain proportions
*   **16:10:** Slight vertical adjustment
*   **4:3:** Elements positioned closer to center to avoid excessive stretching

### Safe Zones

Critical information stays within TV-safe zones (10% margin from edges) for potential console ports:

*   Speedometer, lap counter, position indicator remain readable
*   Optional: "Safe Zone Overlay" to preview safe area in settings

### Dynamic Scaling

*   **4K (3840x2160):** Text sizes scale up for readability
*   **1080p (1920x1080):** Standard text sizes
*   **720p (1280x720):** Text sizes scale down but remain readable
*   **Auto-Detection:** Automatically adjusts based on resolution

---

## Performance Considerations

### Update Frequency

*   **High-Frequency Updates:** Speedometer, lap timer, input display (every frame)
*   **Medium-Frequency Updates:** Position indicator (every 0.1s)
*   **Low-Frequency Updates:** Network stats (every 0.5s)

### Widget Complexity

*   Minimize nested widgets to reduce draw calls
*   Use simple shapes and fonts for performance
*   Avoid real-time blur effects (use pre-blurred backgrounds)

### Memory Footprint

*   Lazy-load conditional elements (don't create pause menu until needed)
*   Unload race results overlay when returning to lobby
*   Cache font glyphs for common characters (numbers, letters)

### GPU Cost

*   **Target:** <1ms GPU time for entire HUD
*   **Optimization:**
    *   Batch draw calls where possible
    *   Use texture atlases for icons
    *   Minimize overdraw with tight bounds

---

## Accessibility Features

### Readability

*   High-contrast text (white on dark backgrounds)
*   Outlined text for visibility against any background
*   Large font sizes for critical information

### Colorblind Support

*   Position indicators use shapes in addition to colors (future)
    *   1st: Gold circle
    *   2nd: Silver square
    *   3rd: Bronze triangle
*   Delta times use +/- symbols in addition to colors

### Motion Sensitivity

*   Minimal animations (avoid excessive motion)
*   Option to disable non-essential animations
*   Stable, fixed positions for all elements

---

## Future Enhancements

*   **Customizable Layouts:** Drag-and-drop HUD editor
*   **Telemetry Widgets:** Advanced data displays (tire temps, brake temps, fuel)
*   **Virtual Rearview Mirror:** Picture-in-picture rear camera view
*   **Voice Notifications:** Audio callouts for lap times, positions
*   **Augmented Reality HUD (VR):** 3D HUD elements for VR displays
*   **Leaderboard Widget:** Live standings during race
*   **Flag System:** Blue flags, yellow flags, checkered flag indicators
