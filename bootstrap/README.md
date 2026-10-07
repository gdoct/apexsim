# game bootstrap executable

This executable is responsible for initializing and launching the game. It sets up the necessary environment, checks for required dependencies, and ensures that all game components are properly loaded before starting the main game application. 
The sequence below is how the launcher should be used by the user.
```
Game.exe
↓
Show cool screen with game logo and progress bar
↓
"Checking environment.."
"Generating config files..."
"Checking content.."
↓
Buttons become enabled
|   |   |   |
|   |   |   ↓
|   |   |   "Manage content"
|   |   ↓
|   |   "Edit configuration" (network / graphics only)
|   ↓
|   "Launch" button with dropdown option
↓
"Troubleshooting..."
```

Main concern: startup speed. This launcher should launch almost instantly.


## manage content: 
shows a tabbed interface with what is inside the content folder: tracks, cars, hud, wheels
has an "import.." button to import AC cars and tracks

## edit configuration:
allows the user to modify network and graphics settings for the game.

## launch:
normal click launches the game. it has a dropdown that reveals a button "Launch with local server"

## troubleshooting:
opens the system default web browser to gdoct.github.io/apexsim for troubleshooting information.