# UI Widget Setup Instructions

Now that the C++ classes are created, you need to create the UMG widgets in the Unreal Editor:

## 1. Compile the C++ Code

In Unreal Editor:
1. Click **Tools → Live Coding → Compile**
   OR
2. Close the editor and build in Visual Studio (F7), then reopen

## 2. Create Loading Screen Widget

1. In **Content Browser**, navigate to `Content/UI/`
2. Right-click → **Add/Import Content → User Interface → Widget Blueprint**
3. Name it `WBP_LoadingScreen`
4. Double-click to open the Widget Blueprint Editor
5. Design the loading screen:
   - Add a **Canvas Panel** as root
   - Add an **Image** widget (for background) - set it to fill the screen
   - Add a **Text** widget with "Loading..." or "ApexSim"
   - Add a **Circular Throbber** or **Progress Bar** for loading animation
   - Style with colors: Dark background, white/blue text

## 3. Create Main Menu Widget

1. In **Content Browser**, navigate to `Content/UI/`
2. Right-click → **Add/Import Content → User Interface → Widget Blueprint**
3. Name it `WBP_MainMenu`
4. Double-click to open the Widget Blueprint Editor
5. Design the main menu:
   - Add a **Canvas Panel** as root
   - Add a **Vertical Box** for menu buttons
   - Add **Buttons** with **Text** labels:
     - "Create Session"
     - "Join Session"
     - "Settings"
     - "Exit"
   - Style the buttons with appropriate colors and fonts

## 4. Create Blueprint GameInstance

1. In **Content Browser**, navigate to `Content/Blueprints/`
2. Right-click → **Blueprint Class**
3. Search for and select `ApexSimGameInstance` as parent
4. Name it `BP_ApexSimGameInstance`
5. Open it and set the class defaults:
   - **Loading Screen Widget Class**: Select `WBP_LoadingScreen`
   - **Main Menu Widget Class**: Select `WBP_MainMenu`
   - **Loading Screen Duration**: 2.0 (or adjust as needed)
6. Compile and Save

## 5. Update Project Settings

1. Go to **Edit → Project Settings**
2. Navigate to **Maps & Modes**
3. Set **Game Instance Class** to `BP_ApexSimGameInstance`
4. Ensure **Editor Startup Map** and **Game Default Map** are set to `MainMenu` (we'll create this next)

## 6. Create MainMenu Map

1. Go to **File → New Level**
2. Select **Empty Level**
3. Save it as `Content/Maps/MainMenu`
4. Add minimal lighting:
   - Add a **Directional Light**
   - Add a **Sky Light**
   - Add a **Sky Atmosphere**
5. Set this as the default map in Project Settings if not already set

## 7. Test the Implementation

1. Click **Play** in the editor
2. You should see:
   - Loading screen appears first
   - After 2 seconds, main menu appears
   - Mouse cursor should be visible on main menu

## 8. Optional: Add Button Functionality

In `WBP_MainMenu`:
1. Select each button
2. In the **Details** panel, scroll to **Events**
3. Click **+** next to **On Clicked**
4. In the Event Graph, you can add placeholder functionality:
   - **Exit Button**: Call `Quit Game` node
   - Other buttons: Add `Print String` nodes for now

---

Save all your work and test by pressing Play in the editor!
