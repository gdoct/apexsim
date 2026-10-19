#pragma once

#include "CoreMinimal.h"

/**
 * Holds the startup splash on screen past the engine's own, with the game
 * window hidden behind it, until the game says the menu is ready.
 *
 * The engine creates and shows its game window during pre-init and closes its
 * splash on the first frame; the menu, the connection and the demo race come
 * after that. This module loads before the window is created and, on Windows:
 *
 *  - opens a copy of the splash (the same Splash.bmp, at the engine splash's
 *    own rectangle) just beneath the engine's, so the engine closing its
 *    splash changes nothing on screen;
 *  - cloaks the game window through DWM as it is created. A cloaked window is
 *    shown as far as the engine and Slate know - it ticks, renders, lays out
 *    and takes its resize - but the compositor does not draw it.
 *
 * The hold must be claimed (Claim) before the first engine frame, or it lets
 * go by itself on that frame; a claimed hold still lets go after
 * WatchdogSeconds, so a game that never calls Reveal cannot stay invisible.
 *
 * Nothing is held in the editor, in commandlets, with -nullrhi, -nosplash or
 * -ApexNoSplashHold, or with -ApexNoDemo / -ApexAutoRace (nothing to wait for).
 */
namespace ApexStartupSplash
{
	/** True from module startup until the splash is gone. */
	APEXSIMBOOT_API bool IsHolding();

	/** Take over the hold; the caller must end it with Reveal or Release. */
	APEXSIMBOOT_API void Claim();

	/**
	 * Make sure this window, and not another, is the one cloaked (the engine may
	 * have created more than one top-level window by the time it has a viewport).
	 * Returns false when it could not be hidden, in which case holding is pointless.
	 */
	APEXSIMBOOT_API bool ConfirmGameWindow(void* OsWindowHandle);

	/** Show the game window beneath the splash and fade the splash out over it. */
	APEXSIMBOOT_API void Reveal(float FadeSeconds);

	/** Show the game window and close the splash at once. */
	APEXSIMBOOT_API void Release();
}
