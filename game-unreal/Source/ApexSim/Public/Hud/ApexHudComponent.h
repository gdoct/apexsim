#pragma once

#include "CoreMinimal.h"
#include "Framework/Text/TextLayout.h"
#include "Hud/ApexHudExpression.h"
#include "Layout/Margin.h"
#include "Types/SlateEnums.h"

/**
 * A HUD component as read from `content/hud/<default|custom>/<id>/component.json`:
 * where it sits on screen and the tree of elements it draws, with every
 * dynamic attribute compiled to an FApexHudExpr. docs/HUD_MODDING.md is the
 * format's reference.
 */
enum class EApexHudElementType : uint8
{
	Row,
	Column,
	Stack,
	Panel,
	Text,
	/** Text in the caption style: small mono capitals, letter-spaced, muted. */
	Label,
	/** A filled rectangle (a light, a bar segment, a damage zone). */
	Rect,
	/** A progress bar, filled from `value` (0-1). */
	Bar,
	Spacer,
	Divider,
	KeyCap,
	/** A PNG from the component's folder. */
	Image,
	/** The circuit outline with a dot per car. */
	Minimap,
	/** The virtual rear-view mirror, while the setting is on. */
	Mirror,
};

/** An attribute: unset, fixed, or worked out every frame. */
struct FApexHudProp
{
	TSharedPtr<const FApexHudExpr> Expr;

	bool IsSet() const { return Expr.IsValid(); }
	bool IsDynamic() const { return Expr.IsValid() && !Expr->IsConstant(); }
};

/** `"repeat": 30` or `{ "count": 30 }`, or `{ "list": "standings", "max": 5, "focus": "is_local" }`. */
struct FApexHudRepeat
{
	int32 Count = 0;
	FName List;
	/** How many rows a list repeat draws at most; the rest are left out. */
	int32 Max = 0;
	/** A list repeat scrolls to keep the first row whose `Focus` field is true in the middle. */
	FName Focus;

	bool IsSet() const { return Count > 0 || !List.IsNone(); }
	bool IsList() const { return !List.IsNone(); }
	/** Widgets the repeat builds. */
	int32 Slots() const { return IsList() ? Max : Count; }
};

struct APEXSIM_API FApexHudElementDef
{
	EApexHudElementType Type = EApexHudElementType::Column;
	/** Where it is in the file, for messages: `root/children[2]`. */
	FString Path;

	// Placement in the parent.
	FMargin Margin;
	TOptional<EHorizontalAlignment> HAlign;
	TOptional<EVerticalAlignment> VAlign;
	float Fill = 0.0f;
	float Width = -1.0f;
	float Height = -1.0f;

	// Containers.
	FMargin Padding;
	/** How a panel lays out its children: Column (default), Row or Stack. */
	EApexHudElementType Direction = EApexHudElementType::Column;
	TArray<FApexHudElementDef> Children;
	FApexHudRepeat Repeat;

	// Text.
	FString FontFace = TEXT("body");
	float FontSize = 14.0f;
	int32 Tracking = 0;
	bool bUpper = false;
	ETextJustify::Type Justify = ETextJustify::Left;

	// Panels and rectangles.
	float Radius = 0.0f;
	float OutlineWidth = 0.0f;
	bool bVertical = false;

	/** Bar fill direction: right, left, up or down. */
	FString BarDirection = TEXT("right");

	/** Image: the PNG's full path. */
	FString ImageFile;

	FApexHudProp Visible;
	FApexHudProp Text;
	/** Text ink, a rectangle's or bar's fill, an image's tint, the local car on the minimap. */
	FApexHudProp Colour;
	/** A panel's fill, a bar's track. */
	FApexHudProp Background;
	FApexHudProp Outline;
	/** A bar's fill, 0-1. */
	FApexHudProp Value;
	FApexHudProp Bold;
};

struct APEXSIM_API FApexHudComponentDef
{
	/** The folder's name: what a custom component replaces a shipped one by. */
	FString Id;
	FString Name;
	FString Description;
	/** The folder it was read from. */
	FString Folder;
	bool bEnabled = true;
	/**
	 * Shown until the player's layout says otherwise. A component shipped with
	 * `"default_enabled": false` is one the HUD editor offers to add.
	 */
	bool bDefaultEnabled = true;

	/** One of ApexHud::Regions(). */
	FString Region = TEXT("bottom-left");
	/** Order within the region, lowest first (outermost first for the side regions). */
	int32 Order = 0;
	FMargin Margin;
	/** Placed on its own at the region's corner rather than in the region's row, so it moves nothing. */
	bool bFloat = false;
	FApexHudProp Visible;

	FApexHudElementDef Root;
};

/** Messages from loading: errors drop a component, warnings do not. */
struct APEXSIM_API FApexHudLoadReport
{
	TArray<FString> Errors;
	TArray<FString> Warnings;
};

namespace ApexHud
{
	/** The regions a component can sit in: top-left, top, top-right, left, center, right, bottom-left, bottom, bottom-right. */
	APEXSIM_API const TArray<FString>& Regions();

	/**
	 * The folders HUD components are read from, lowest priority first: the
	 * repo's `content/hud` in the editor or `Hud/` beside ApexSim.exe in a
	 * package, then each `-ApexHudDir=<dir>[+<dir>]`.
	 */
	APEXSIM_API TArray<FString> HudDirectories();

	/** Every component folder under the directories, lowest priority first: each directory's `default/` then `custom/`. */
	APEXSIM_API TArray<FString> ComponentFolders(const TArray<FString>& Directories);

	/** JSON with line comments, block comments and trailing commas, made into plain JSON. */
	APEXSIM_API FString StripJsonExtras(const FString& Text);

	/** One component from its file's text. False (with errors) when it cannot be drawn. */
	APEXSIM_API bool ParseComponent(const FString& Text, const FString& Folder, FApexHudComponentDef& Out, FApexHudLoadReport& Report);

	/**
	 * Every component, a later folder replacing an earlier one with the same
	 * id (so `custom/standings` replaces `default/standings`, and one with
	 * `"enabled": false` hides it). Disabled components are left out.
	 */
	APEXSIM_API void LoadComponents(const TArray<FString>& Directories, TArray<FApexHudComponentDef>& Out, FApexHudLoadReport& Report);

	/** Warns about every name an expression reads that the game does not publish: almost always a typo. */
	APEXSIM_API void CheckDataNames(const FApexHudComponentDef& Component, TArray<FString>& OutWarnings);
}
