# Content Management Scene Specification

## Overview

The Content Management scene allows players to browse and select available cars and tracks. In the initial phase, it displays server-provided content. Future versions will support local mod loading and content creation.

---

## Scene Type

*   **Implementation:** UMG Widget Blueprint
*   **Server Interaction:** Receives content lists from `SimNetClient`
*   **Updates:** Reacts to `OnLobbyUpdated` events for content availability

---

## UI Layout

### Two-Panel Design

The scene is divided into two main sections:

1. **Left Panel:** My Cars List
2. **Right Panel:** My Tracks List

### Header

*   **Title:** "Content Management"
*   **Subtitle:** "Browse and select your cars and tracks"

---

## My Cars Panel (Left)

### Car List Display

*   **Title:** "My Cars"
*   **Content:** Scrollable list of available car configurations
*   **Data Source:** `FClientCarConfig` structs from `SimNetClient`

### Car Entry Display

Each car entry shows:

*   **Car Name:** Display name from `FClientCarConfig.Name`
*   **Car Preview:**
    *   Thumbnail image (future feature)
    *   Placeholder colored box with car initial letter
*   **Selection Indicator:** Checkmark or highlight if this car is currently selected
*   **Stats Display (Future):**
    *   Power: "XXX HP"
    *   Weight: "XXX kg"
    *   Class: "GT3" / "Formula" / etc.

### Car Actions

*   **"Select Car" Button:**
    *   Appears on hover or when entry is focused
    *   Function: Sends `ClientMessage::SelectCar` with `FCarConfigId`
    *   Visual feedback: Updates selection indicator on success
    *   Disabled if already selected

*   **"View Details" Button (Future):**
    *   Opens detailed car info panel
    *   Shows full specifications, handling characteristics
    *   3D preview rotation

### Empty State

If no cars available:
*   Display message: "No cars available"
*   Show "Refresh" button to re-query server

---

## My Tracks Panel (Right)

### Track List Display

*   **Title:** "My Tracks"
*   **Content:** Scrollable list of available track configurations
*   **Data Source:** `FClientTrackConfig` structs from `SimNetClient`

### Track Entry Display

Each track entry shows:

*   **Track Name:** Display name from `FClientTrackConfig.Name`
*   **Track Preview:**
    *   Thumbnail image (future feature)
    *   Placeholder colored box with track initial letter
*   **Track Info (Future):**
    *   Length: "5.2 km"
    *   Turns: "18"
    *   Layout variant: "GP Circuit" / "Short" / etc.

### Track Actions

*   **"View Details" Button (Future):**
    *   Opens detailed track info panel
    *   Shows track map, sector breakdown
    *   Track records (future feature)

### Empty State

If no tracks available:
*   Display message: "No tracks available"
*   Show "Refresh" button to re-query server

---

## Footer Actions

### Back Button

*   **Label:** "Back to Main Menu"
*   **Position:** Bottom-left
*   **Function:** Returns to Main Menu scene

### Refresh Button (Future)

*   **Label:** "Refresh Content"
*   **Position:** Bottom-center
*   **Function:** Manually refreshes content lists from server

### Add Content Button (Future - Local Mods)

*   **Label:** "Add Custom Content"
*   **Position:** Bottom-right
*   **Function:** Opens file browser to add local car/track mods

---

## Logic & Behavior

### Loading Content

#### On Scene Enter
1. Check if content lists already cached in `SimNetClient`
2. If not cached, request content lists from server
3. Display loading spinner while waiting
4. Populate lists when data received

#### Receiving Content Updates
*   Listen for `SimNetClient`'s `OnLobbyUpdated` event
*   Update car list if `LobbyUpdate.available_cars` changes
*   Update track list if `LobbyUpdate.available_tracks` changes
*   Maintain scroll position during updates

### Selecting a Car

1. User clicks "Select Car" on a car entry
2. Send `ClientMessage::SelectCar` with selected `FCarConfigId`
3. On success:
    *   Update local player state in `SimNetClient`
    *   Move selection indicator to new car
    *   Show confirmation toast: "Car selected: [Car Name]"
4. On failure:
    *   Show error toast: "Failed to select car: [reason]"
    *   Keep previous selection

### Visual Feedback

*   **Hover Effect:** Scale slightly on mouse hover (1.0x → 1.02x)
*   **Selected State:** Green border or checkmark overlay
*   **Loading State:** Show skeleton placeholders while loading
*   **Error State:** Red border and error icon if content fails to load

### Filtering & Sorting (Future)

*   **Sort By:** Name (A-Z), Power (high to low), Class
*   **Filter By:** Car class, manufacturer
*   **Search:** Text input to filter by name

---

## Future Features: Local Mod Loading

### Mod Directory Structure

```
%AppData%/ApexSim/Mods/
├── Cars/
│   ├── CustomCar1/
│   │   ├── car_config.json
│   │   ├── model.fbx
│   │   ├── textures/
│   │   └── sounds/ (optional)
│   └── CustomCar2/
└── Tracks/
    ├── CustomTrack1/
    │   ├── track_config.json
    │   ├── model.fbx
    │   └── textures/
    └── CustomTrack2/
```

### Mod Loading Process

1. Scan mod directories on scene load
2. Validate configuration files (`car_config.json`, `track_config.json`)
3. Load asset references (meshes, textures) via UE's `AssetRegistry`
4. Add valid mods to content lists with "Local Mod" badge
5. Handle loading errors gracefully (skip invalid mods, log errors)

### Mod Management UI

*   **"Add Mod" Button:** Opens file browser to import mod packages
*   **"Remove Mod" Button:** Deletes local mod files (with confirmation)
*   **"Mod Details" Panel:** Shows mod author, version, description
*   **"Enable/Disable" Toggle:** Temporarily disable mods without deleting

### Content Validation

*   **Required Files:** Config file + mesh file minimum
*   **Format Validation:** JSON schema validation for config files
*   **Asset Validation:** Check that referenced assets exist and load correctly
*   **Server Compatibility:** Ensure physics config matches server expectations (future)

---

## Accessibility

*   **Keyboard Navigation:** Arrow keys to navigate lists, Enter to select
*   **Controller Navigation:** D-pad/stick navigation, A/X to select
*   **Screen Reader:** Announce car/track names and selection state
*   **High Contrast:** Ensure selection indicators work in high-contrast mode

---

## Performance Considerations

*   **List Virtualization:** Use UE's list view virtualization for large content libraries
*   **Lazy Loading:** Load thumbnails on-demand as user scrolls
*   **Caching:** Cache content lists locally to reduce server queries
*   **Async Asset Loading:** Load mod assets asynchronously to prevent UI freezing

---

## Error Handling

*   **Server Unreachable:** Display "Unable to load content" with retry button
*   **Invalid Mod:** Show warning banner listing invalid mods with details
*   **Asset Load Failure:** Display placeholder graphics, continue loading other content
*   **Selection Conflict:** If car no longer available, show error and revert to previous selection
