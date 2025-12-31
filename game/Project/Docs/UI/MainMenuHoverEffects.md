# Adding Hover Effects to Main Menu Buttons

## Overview

This guide explains how to add hover effects to your main menu buttons using the new C++ `UMainMenuWidget` class. The hover effects include:
- Scale animation (1.0x ? 1.05x by default)
- Color tint on hover
- Optional sound effects

## Step 1: Compile C++ Code

1. **In Visual Studio:**
   - Build Solution (F7) or Ctrl+Shift+B
   
2. **Or in Unreal Editor:**
   - Tools ? Live Coding ? Compile

## Step 2: Update WBP_MainMenu Blueprint

Since you cannot use headless mode for this, you'll need to open the editor once:

1. Open Unreal Editor
2. Navigate to `Content/UI/WBP_MainMenu`
3. Open the Widget Blueprint
4. In the **File ? Reparent Blueprint** menu:
   - Search for `MainMenuWidget`
   - Select it as the new parent class
5. Save the Blueprint

## Step 3: Name Your Buttons Correctly

The C++ class expects buttons with specific names. In `WBP_MainMenu`:

1. Select each button in the hierarchy
2. Rename them in the Details panel:
   - First button ? `PlayButton`
   - Second button ? `SettingsButton`
   - Third button ? `ContentButton`
   - Fourth button ? `QuitButton`

**Important:** The names must match exactly (case-sensitive) for the automatic binding to work.

## Step 4: Configure Hover Effect Properties (Optional)

In the `WBP_MainMenu` Blueprint:

1. Select the root widget (should show MainMenuWidget properties)
2. In the **Details** panel, find the "ApexSim|UI|HoverEffects" category
3. Adjust these properties as desired:
   - **Hover Scale Multiplier:** Default 1.05 (5% larger on hover)
   - **Hover Animation Duration:** Default 0.15 seconds
   - **Hover Color Tint:** Default (1.2, 1.2, 1.2, 1.0) - slightly brighter
   - **Play Sound On Hover:** true/false
   - **Hover Sound:** Assign a sound asset if desired

## Step 5: Test

1. Play in Editor (PIE)
2. Hover over the menu buttons
3. You should see:
   - Buttons scale up smoothly
   - Buttons become slightly brighter
   - Sound plays (if configured)

## Alternative: Manual Button Binding (If Names Don't Match)

If you don't want to rename your buttons, you can bind them manually:

1. Open `WBP_MainMenu` in the Widget Blueprint Editor
2. Go to the **Event Graph**
3. Override the `Construct` event
4. After calling `Parent: Construct`, add nodes to:
   - Get references to your buttons
   - Call `Bind Button Hover Effects` (from the C++ class)

## Customizing Hover Effects

### Option 1: Modify in Blueprint
You can override the `ApplyHoverEffect` and `RemoveHoverEffect` functions in your Blueprint to create custom animations.

### Option 2: Modify in C++
Edit `Source/ApexSim/UI/MainMenuWidget.cpp`:
- `ApplyHoverEffect()` - Controls what happens on hover
- `RemoveHoverEffect()` - Controls what happens when hover ends

## Features Included

? Automatic button detection and binding  
? Smooth scale animation on hover  
? Color tint effect  
? Optional sound playback  
? Preserves original button appearance when not hovered  
? Configurable through Blueprint properties  

## Troubleshooting

**Buttons don't respond to hover:**
- Check that button names match exactly (PlayButton, SettingsButton, etc.)
- Ensure WBP_MainMenu's parent class is `MainMenuWidget`
- Verify buttons have "Is Focusable" enabled in their properties

**Hover effect looks wrong:**
- Adjust `HoverScaleMultiplier` and `HoverColorTint` in the Details panel
- Check that buttons have proper initial colors/transforms set

**Compile errors:**
- Make sure UMG module is in your Build.cs (already added)
- Rebuild the project completely

## Next Steps

You can extend this system to:
- Add click animations
- Add button press effects
- Create different hover styles for different button types
- Add particle effects on hover
- Integrate with a UI animation system

---

**Note:** After the initial setup in the editor, all hover behavior is controlled by C++ code, which can be modified without opening the editor.
