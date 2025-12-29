# Main Menu Scene Specification

## Overview

The Main Menu is the entry point for the application, providing navigation to all major game features. It is implemented as a UMG Widget Blueprint and serves as the primary hub for player interaction.

---

## Scene Type

*   **Implementation:** Primarily UMG Widget Blueprint
*   **Loading:** Can be loaded as a Level or set as the default startup widget
*   **Persistence:** Remains in memory during session for quick navigation

---

## UI Components

### Main Menu Container

*   **Layout:** Vertical box centered on screen
*   **Background:** Full-screen background image/video (track footage or abstract racing theme)
*   **Overlay:** Semi-transparent dark gradient for text readability

### Logo/Title

*   **Position:** Top-center
*   **Content:** "ApexSim" title text or logo image
*   **Style:** Large, bold, high-contrast

### Navigation Buttons

All buttons use consistent styling with hover effects and click feedback:

#### "Play" Button
*   **Function:** Navigates to the Create/Join Session screen
*   **Position:** Primary position in vertical list
*   **Shortcut:** Enter key (when menu focused)

#### "Settings" Button
*   **Function:** Navigates to the Settings screen
*   **Position:** Second in vertical list
*   **Shortcut:** Configurable

#### "Content" Button
*   **Function:** Navigates to the Content Management screen
*   **Position:** Third in vertical list
*   **Shortcut:** Configurable

#### "Quit" Button
*   **Function:** Exits the application
*   **Position:** Bottom of vertical list
*   **Shortcut:** ESC (with confirmation dialog)
*   **Behavior:** Shows confirmation dialog: "Are you sure you want to quit?"

### Version Information

*   **Position:** Bottom-right corner
*   **Content:** Application version number (e.g., "v0.1.0-alpha")
*   **Style:** Small, subtle text

---

## Logic & Behavior

### Navigation Flow

```
Main Menu
├── Play → CreateJoinSession Scene
├── Settings → Settings Scene
├── Content → ContentManagement Scene
└── Quit → Confirmation Dialog → Exit Application
```

### No Server Interaction

*   The Main Menu does not communicate with the server
*   All functionality is client-side UI navigation
*   Server connection is established when entering CreateJoinSession

### State Management

*   Menu state persists across scene changes
*   Returns to Main Menu restore previous selection highlight
*   Background continues playing during transitions

### Transition Effects

*   **Scene Changes:** Smooth fade-out/fade-in (0.3s duration)
*   **Button Interactions:** Scale animation on hover (1.0x → 1.05x)
*   **Audio:** Play UI navigation sounds (see [Audio System](../Systems/AudioSystem.md))

---

## Accessibility

*   **Keyboard Navigation:** Full support for arrow keys + Enter
*   **Controller Navigation:** Full support with consistent button mapping
*   **Screen Reader:** Proper ARIA labels for UI elements (future feature)
*   **High Contrast Mode:** Support for high-contrast text themes (future feature)

---

## Performance Considerations

*   **Memory Footprint:** Keep widget complexity low for fast loading
*   **Background Video:** Optional, can be disabled in low-performance mode
*   **Preloading:** Preload next scene assets when button is hovered (future optimization)

---

## Future Enhancements

*   **News/Updates Panel:** Display server news or patch notes
*   **Quick Join:** Direct "Quick Race" button for instant matchmaking
*   **Friends List:** Integration with social features
*   **Replays:** Direct access to replay browser
*   **Statistics:** Career stats summary display
