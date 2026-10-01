#pragma once

#include "CoreMinimal.h"
#include "Layout/SlateRect.h"

/**
 * Where the player has put one HUD component, from the HUD editor.
 *
 * A component with no placement sits where its component.json says (its
 * region and order). One that has been moved is *pinned*: its `Anchor`
 * corner (0-1 on each axis: 0,0 top left, 1,1 bottom right, 0.5 the middle)
 * sits `Position` units from the same point of the screen, so a panel moved
 * to the bottom right stays in the bottom right at any resolution. Units are
 * the HUD's: pixels on a 1080p screen, scaled with the UI.
 */
struct APEXSIM_API FApexHudPlacement
{
	bool bEnabled = true;
	bool bPinned = false;
	FVector2D Anchor = FVector2D::ZeroVector;
	FVector2D Position = FVector2D::ZeroVector;
	float Scale = 1.0f;

	bool operator==(const FApexHudPlacement& Other) const
	{
		return bEnabled == Other.bEnabled && bPinned == Other.bPinned && Anchor == Other.Anchor
			&& Position == Other.Position && Scale == Other.Scale;
	}
};

/**
 * The player's arrangement of the HUD: per component id, whether it is shown
 * and where. Kept in `custom/layout.json` beside the components (so a layout
 * can be shared like a component), written by the HUD editor in the settings.
 */
struct APEXSIM_API FApexHudLayout
{
	TMap<FString, FApexHudPlacement> Components;

	static constexpr float MinScale = 0.5f;
	static constexpr float MaxScale = 2.0f;

	const FApexHudPlacement* Find(const FString& Id) const { return Components.Find(Id); }

	/** Whether the component is shown: the layout's say, else its own `default_enabled`. */
	bool IsEnabled(const FString& Id, bool bDefaultEnabled) const
	{
		const FApexHudPlacement* Placement = Find(Id);
		return Placement ? Placement->bEnabled : bDefaultEnabled;
	}

	float ScaleOf(const FString& Id) const
	{
		const FApexHudPlacement* Placement = Find(Id);
		return Placement ? Placement->Scale : 1.0f;
	}

	FString ToJson() const;
	/** False (with OutError) when the text is not a layout; a bad entry is skipped with a warning in OutError. */
	bool FromJson(const FString& Text, FString& OutError);

	/** `<first HUD directory>/custom/layout.json`. */
	static FString DefaultFile();
	/** An absent file is an empty layout, not an error. */
	bool Load(const FString& File, FString& OutError);
	bool Save(const FString& File) const;
};

namespace ApexHudPlace
{
	/**
	 * Pins a component whose rectangle on a screen of `ScreenSize` is `Rect`
	 * (HUD units), to the nearest of the nine anchor points: the screen is
	 * cut in thirds and the rectangle's centre decides. Nothing moves.
	 */
	APEXSIM_API FApexHudPlacement PinRect(const FSlateRect& Rect, const FVector2D& ScreenSize, float Scale, bool bEnabled = true);

	/** Where a pinned component of `Size` lands on a screen of `ScreenSize`: its top-left corner. */
	APEXSIM_API FVector2D TopLeft(const FApexHudPlacement& Placement, const FVector2D& Size, const FVector2D& ScreenSize);

	/** The scale held to the editor's range and its 5% steps. */
	APEXSIM_API float ClampScale(float Scale);

	/** The lines a dragged component snaps to, found by Snap. */
	struct FSnapLines
	{
		TOptional<float> X;
		TOptional<float> Y;
	};

	/**
	 * Moves `Rect` by at most `Distance` so an edge or its centre meets one of
	 * the guide lines: the screen's gutters (56 from the sides, 30 from top
	 * and bottom), its centre lines, and every edge and centre of `Others`.
	 * Returns the snapped rectangle; `OutLines` says which line it met on each axis.
	 */
	APEXSIM_API FSlateRect Snap(const FSlateRect& Rect, const FVector2D& ScreenSize, const TArray<FSlateRect>& Others,
		float Distance, FSnapLines& OutLines);
}
