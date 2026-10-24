#pragma once

#include "CoreMinimal.h"

/**
 * The garage's livery editor (N11 phase 2; apexsim-content/docs/cars.md
 * "The livery editor"): a paint and an accent colour from a set of
 * swatches, a race number drawn onto the `car_logo` texture, and a name,
 * saved as one of the player's own liveries
 * (`cars/custom/liveries/<car folder>/<folder>/livery.toml` and `logo.png`).
 * The car select screen draws the panel; what it saves and draws is here.
 */
struct FApexLiveryDraft
{
	FString Name;
	/** Indices into ApexLiveryEditor::Swatches(). */
	int32 Paint = 0;
	int32 Accent = 1;
	/** The race number, 0..99; -1 for none (no logo). */
	int32 Number = -1;
};

namespace ApexLiveryEditor
{
	struct FSwatch
	{
		const TCHAR* Name;
		/** sRGB, as a colour picker shows it; the livery stores it linear. */
		FColor Srgb;
	};

	/** The editor's colours, in the order the rows step through them. */
	APEXSIM_API TConstArrayView<FSwatch> Swatches();

	/** A swatch's colour as livery.toml stores it: linear RGB. */
	APEXSIM_API FLinearColor Linear(int32 Swatch);

	/** "<paint> <number>" ("Racing red 17"): the name a draft has until the player types one. */
	APEXSIM_API FString AutoName(const FApexLiveryDraft& Draft);

	/** The livery's name as livery.toml can hold it: no quotes, backslashes or control characters, 40 characters at most. */
	APEXSIM_API FString CleanName(const FString& Name);

	/** A folder for `Name` under `Root` that is not taken yet: lower case, letters, digits and dashes ("racing-red-17", "racing-red-17-2"). */
	APEXSIM_API FString FolderFor(const FString& Name, const FString& Root);

	/** The livery.toml of a draft: one `[[livery]]` table, its logo `logo.png` when it has one. */
	APEXSIM_API FString LiveryToml(const FApexLiveryDraft& Draft, bool bLogo);

	/**
	 * Saves the draft as a new own livery of the car in `CarDir`
	 * (ApexLivery::OwnLiveryRoot): its folder, livery.toml and, when
	 * `LogoPng` is not empty, logo.png. OutFolder is the folder made.
	 */
	APEXSIM_API bool Save(const FString& CarDir, const FApexLiveryDraft& Draft, TArrayView<const uint8> LogoPng,
		FString& OutFolder, FString& OutError);

	/** The logo texture's size, as the works logos are drawn (tools/generators/cars/liveries.py). */
	inline constexpr int32 LogoWidth = 1536;
	inline constexpr int32 LogoHeight = 512;

	/**
	 * The logo's pixels from the number's glyph coverage (0..255 per pixel,
	 * LogoWidth x LogoHeight): a rounded plate in `Plate`'s colour, the number
	 * on it in black or white, whichever reads, the rest transparent.
	 */
	APEXSIM_API TArray<FColor> ComposeLogo(TArrayView<const uint8> Coverage, const FLinearColor& Plate);

	/**
	 * The number's logo as PNG bytes: the glyphs drawn by a canvas into a
	 * render target, then composed (ComposeLogo). False without a renderer
	 * (-nullrhi): the livery then has its colours and no number.
	 */
	APEXSIM_API bool RenderNumberLogo(int32 Number, const FLinearColor& Plate, TArray<uint8>& OutPng);
}
