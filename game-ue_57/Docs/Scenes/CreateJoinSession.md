# Create/Join Session Scene Specification

## Overview

The Create/Join Session scene is the lobby interface where players can see other online players, view available racing sessions, create new sessions, and join existing ones. This scene handles all pre-race matchmaking and session management.

---

## Scene Type

*   **Implementation:** Primarily UMG Widget Blueprint
*   **Server Interaction:** Heavy - displays live data from server via `SimNetClient`
*   **Updates:** Real-time updates via `OnLobbyUpdated` events

---

## UI Components

### Scene Layout

The scene is divided into three main panels:

1. **Left Panel:** Lobby Player List
2. **Center Panel:** Available Sessions List
3. **Right Panel:** Session Actions

### Lobby Player List (Left Panel)

*   **Title:** "Players in Lobby"
*   **Content:** Scrollable list of all connected players
*   **Player Entry Display:**
    *   Player name (FString from `FClientPlayer`)
    *   Online status indicator (green dot)
    *   Currently selected car icon (optional, future feature)
*   **Data Source:** Received via `SimNetClient`'s `LobbyUpdate` message
*   **Sorting:** Alphabetical by player name

### Available Sessions List (Center Panel)

*   **Title:** "Available Sessions"
*   **Content:** Scrollable list of all active racing sessions
*   **Session Entry Display:**
    *   Track name (resolved from `FTrackConfigId`)
    *   Host player name
    *   Player count: "X / Y players" (current / max)
    *   Session state badge:
        *   "Waiting" (green) - `SessionState::Lobby`
        *   "Countdown" (yellow) - `SessionState::Countdown`
        *   "Racing" (red) - `SessionState::Active`
        *   "Finished" (gray) - `SessionState::Finished`
    *   Lap limit: "X laps"
    *   "Join" button (disabled if session is Active or full)
*   **Data Source:** Received via `SimNetClient`'s `LobbyUpdate` message
*   **Sorting:** By creation time (newest first) or number of players (most populated first)
*   **Filtering:** Option to hide full sessions or in-progress races

### Session Actions (Right Panel)

#### Create Session Button

*   **Label:** "Create New Session"
*   **Function:** Opens Create Session popup dialog
*   **Enabled:** Always available

#### Create Session Popup

*   **Modal:** Yes (blocks background interaction)
*   **Fields:**
    *   **Track Selection** (Dropdown)
        *   Lists all available tracks from `FClientTrackConfig`
        *   Display track name
        *   Default: First track in list
    *   **Max Players** (Number input / Slider)
        *   Range: 2-20 players
        *   Default: 8 players
    *   **Lap Limit** (Number input / Slider)
        *   Range: 1-100 laps
        *   Default: 5 laps
*   **Buttons:**
    *   **"Create":** Sends `ClientMessage::CreateSession` via `SimNetClient`
    *   **"Cancel":** Closes popup without action
*   **Validation:**
    *   All fields must have valid values
    *   Show error message if validation fails

#### Back Button

*   **Label:** "Back to Main Menu"
*   **Position:** Bottom of right panel
*   **Function:** Returns to Main Menu scene
*   **Confirmation:** If player is in a session, show confirmation dialog

---

## Logic & Behavior

### Server Communication

#### On Scene Enter
1. Connect to server (if not already connected)
2. Send `ClientMessage::EnterLobby` or equivalent
3. Request current lobby state
4. Subscribe to lobby update events

#### Receiving Lobby Updates
*   Listen for `SimNetClient`'s `OnLobbyUpdated` event
*   Update player list with `LobbyUpdate.players` data
*   Update session list with `LobbyUpdate.sessions` data
*   Handle player join/leave animations (fade in/out)
*   Handle session state changes (update badges, enable/disable join buttons)

#### Creating a Session
1. User fills out Create Session popup
2. On "Create" click, validate input
3. Send `ClientMessage::CreateSession` with:
    *   Selected `FTrackConfigId`
    *   Max players count
    *   Lap limit
4. On success:
    *   Close popup
    *   Show confirmation toast: "Session created!"
    *   Player is automatically joined to new session
    *   Transition to Session Lobby view (future) or remain in CreateJoinSession
5. On failure:
    *   Show error message in popup
    *   Keep popup open for correction

#### Joining a Session
1. User clicks "Join" button on a session entry
2. Send `ClientMessage::JoinSession` with `FSessionId`
3. On success:
    *   Show confirmation toast: "Joined session!"
    *   Update UI to reflect player is now in session
    *   Session entry shows "+1 player"
4. On failure (session full, already started, etc.):
    *   Show error toast: "Failed to join: [reason]"
    *   Refresh session list

### State Management

#### Player Session State
*   **Not in Session:** Can create or join any available session
*   **In Session:**
    *   Cannot join another session (must leave first)
    *   Can see session details in a "Current Session" panel (future feature)
    *   "Leave Session" button available

### Error Handling

*   **Connection Lost:** Display prominent "Connection to server lost" overlay
    *   Attempt auto-reconnect (3 retries)
    *   "Retry" and "Back to Main Menu" buttons
*   **Session No Longer Available:** Remove from list, show toast notification
*   **Server Full:** Show error when attempting to join

### Real-Time Updates

*   Player list updates in real-time as players connect/disconnect
*   Session list updates as sessions are created/destroyed/change state
*   Update frequency: Driven by server push events (no polling)

---

## Accessibility

*   **Keyboard Navigation:** Tab through lists and buttons
*   **Controller Navigation:** D-pad/stick to navigate, A/X to select
*   **Screen Reader:** Announce player joins/leaves and session updates
*   **Color Coding:** State badges use both color AND text for colorblind users

---

## Performance Considerations

*   **List Virtualization:** Use UE's list view virtualization for large player/session lists
*   **Update Throttling:** Batch rapid lobby updates (max 10 updates/second)
*   **Memory:** Limit stored lobby history (keep only current state)

---

## Future Enhancements

*   **Session Details View:** Click session to see full details (player list, rules, etc.)
*   **Session Chat:** Text chat while in lobby
*   **Filters/Search:** Filter sessions by track, player count, state
*   **Friends Highlighting:** Highlight friends in player list
*   **Private Sessions:** Password-protected sessions
*   **Session Templates:** Quick-create with preset configurations
*   **Spectator Mode:** Join session as spectator
