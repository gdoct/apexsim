# Feature: reorganize main menu

in the unreal game, reorganize the main menu. the main options should be "Garage", "Drive", "Watch a race", and "Settings"

## Garage
Garage is where the player manages their cars and equipment.
* Garage
  - manage car setups
  - view car stats
* Replays
  (shows list with existing replays with watch, delete and export options)
* Tracks
  - watch hotlap
  - see track guide
## Drive
Drive is the driving experience
* Create session
  - quick
  - race weekend
  - custom

* Browse sessions
  - shows list with current and recorded sesssions 
  - player can watch a session live if the session is live
  - player can replay a session
  - player can join a live session
  - player can join a recorded session (by creating a new session from a previous recorded session; player can modify the settings and grid before starting it, and join at a specific timestamp)
  - player can also filter sessions by track, car, and driver, and status (active / archived)

* Connect to server
  - modifies the server address (requires restart)
  - player needs to restart the game for the changes to take effect
  - player can also reset the server address to default (requires restart)

## Watch a race
Watch a race is for viewing random live races (existing functionality in the game)

## Settings
Settings is for configuring the game (existing functionality in the game)

## Implementation status

The buttons on the right of the main menu screen (`ApexMainMenuWidget`) are now the tree above:
the four main entries, with Garage and Drive opening sub-pages (Back row, pad B or the Back
action steps up). The "continue where you left off" hero on the left is unchanged.

* Esc opens "Back to main menu" / "Exit game" (Back returns to the top-level entries); Esc again or pad B closes it.
* Alt+Enter cycles fullscreen / borderless / windowed (through the settings subsystem, so it is saved) and no longer activates the focused row.
* The old greyed "Qualifying" row was dropped from the rail (Qualifying is still a mode on the create screen).

### Garage > Tracks > Watch hotlap

One AI car laps a circuit alone, on its racing line and at the speed its car can manage, full screen, with a minimal HUD. It is worked out by
the server when it is asked for (a `SessionKind::HotlapWatch` session of one AI car, spectated by the player), so it can be asked for under any car, weather,
time of day and circuit, and nothing is stored.

* It starts on the pending circuit and car (the menu's "continue where you left off"), under the weather and hour last watched. The first lap leaves a
  standing start 300 m before the line, so it is a little slower than the laps after it; every lap after starts on fresh tyres and a fresh tank, so a long
  watch does not drift.
* The HUD is the `hotlap_watch` scene (docs/game/HUD_MODDING.md, "Scenes"): the circuit and car, the lap clock with each sector as it closes and the last and
  best laps, the corner the car is in or comes to (number and name), a small gear / speed / revs / pedals widget, and the keys. Nothing else of the race HUD
  is shown. `H` hides it all.
* While watching: Left / Right (pad LB / RB) change the car, `W` (pad X) the weather, `[` and `]` (pad View) the hour, Page Up / Page Down (pad L3) the
  circuit, `C` the camera. A change starts the lap over for the new choice after a moment, so several presses make one new lap. Esc opens the pause menu,
  "Stop watching" returns to the menu.
* The driver is the server's top-skill AI on its exact racing line. A few car and circuit pairs leave no room for that line (a stand against it at Monza's
  start, Suzuka's rails in the GT3): the driver is eased off a step after a crash or a lap struck for leaving the track until the laps are clean, so the
  first laps of those can show LAP INVALID once or twice.
* Corners come from the track guide's detection and the circuit's dossier (`TrackCorners`); a corner the dossier has no name for is "Turn N".
* `-ApexWatchHotlap -ApexTrack=<stem> -ApexCar=<name> -ApexWeather=<sky> -ApexTimeOfDay=HH:MM` starts one at launch, for an unattended run.

### Not implemented yet (greyed out or partial)

| Menu item | State | What is missing |
|---|---|---|
| Garage > Garage > Manage car setups | Opens the hotlap garage's setup sheet over the menu (`UApexHotlapWidget::SetMenuMode`) | Works, but without a session the server's setup sheet is not available, so knobs read in **clicks** rather than real units (and the compound cards use the default five). Not built or run in-game yet. |
| Garage > Garage > View car stats | Opens the car select screen | A dedicated stats view; today it is whatever the car picker shows. |
| Garage > Replays | Opens the existing replays screen | Watch and delete exist; **export** does not. (Keep exists too.) |
| Garage > Tracks > See track guide | Opens the track picker | Guide is opened from there (G / GUIDE chip); no direct jump into a guide. |
| Drive > Create session > Quick | Starts the remembered session | Quick has no settings of its own; it is the hero's "Start session". |
| Drive > Create session > Race weekend | Greyed out ("Not yet") | Race weekend (practice, qualifying, race in sequence) does not exist. |
| Drive > Create session > Custom | Opens the create screen | Complete. |
| Drive > Browse sessions | Opens the existing browser (live sessions: watch / join) | Recorded/archived sessions, replaying a session, joining a recorded session (new session from a recording with edited grid and start timestamp), and filters by track, car, driver and status. |
| Drive > Connect to server | Opens the existing connect dialog | Not verified: the "restart required" notice and a "reset to default address" action. |

when the user is at the main screen and presses esc, the game should show a menu for "back to main menu" and "exit game" options. also, when the user presses alt-enter i this screen, the screen should flip between display modes, not start a game

