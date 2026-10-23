#pragma once

#include "CoreMinimal.h"
#include "ApexSettingsSave.h"

/**
 * The colours that carry meaning on the race HUD, by role, for each colour
 * vision palette (EApexColourVision).
 *
 * The HUD's components name these roles (`live`, `error`, `accent`,
 * `session_best`, `tyre_cold`, `flag_yellow`, ... in ApexHudColour::Parse)
 * and the C++ race widgets (the cockpit dash, the hotlap sheet) read them
 * here, so one setting moves every one of them. The menu's own palette
 * (ApexUI::Palette) is not a role table: it carries no meaning a colour-blind
 * player could miss, and stays as it is.
 *
 * Colour is never the only cue on the shipped HUD: every state a role marks
 * also shows as text, a sign or a shape (docs/game/hud-modding.md, Colour
 * vision). The palettes make the colours themselves tell apart under each
 * deficiency, checked by ApexSim.UI.ColourVision with a simulation of it.
 */
enum class EApexColourRole : uint8
{
	/** The HUD's accent, and the warning step between good and bad. */
	Accent,
	/** Good, on, gaining: green in the standard palette. */
	Good,
	/** Bad, over, losing: red in the standard palette. */
	Bad,
	/** A driver's own best time. */
	PersonalBest,
	/** A time nobody in the session has beaten. */
	SessionBest,
	/** Below the tyre's (or the brake's) working window. */
	TyreCold,
	/** In the tyre's working window. */
	TyreOk,
	FlagYellow,
	FlagBlue,
	/** The formation and pace laps. */
	FlagGreen,
	FlagRed,
	/** The cockpit dash's last row of rev lights. */
	ShiftLight,

	Count
};

namespace ApexColourVision
{
	/** The role's colour in that palette. */
	APEXSIM_API FLinearColor Role(EApexColourRole Role, EApexColourVision Vision);

	/** The role's colour in the palette the player chose. */
	APEXSIM_API FLinearColor Role(EApexColourRole Role);

	/** The palette the race widgets draw with now; Standard until the settings say otherwise. */
	APEXSIM_API EApexColourVision Current();

	/** Set by UApexSettingsSubsystem when the setting is applied. */
	APEXSIM_API void SetCurrent(EApexColourVision Vision);

	/** `standard`, `red_green`, `blue_yellow`: the console's and the docs' names. */
	APEXSIM_API const TCHAR* Name(EApexColourVision Vision);

	/** What the settings segment shows. */
	APEXSIM_API const TCHAR* Label(EApexColourVision Vision);

	/**
	 * A palette from its name, its index, or the deficiency it is for
	 * (`deuteranopia`, `protanopia`, `tritanopia`). Case-insensitive.
	 */
	APEXSIM_API bool Parse(const FString& Text, EApexColourVision& Out);
}
