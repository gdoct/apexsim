#pragma once

#include "CoreMinimal.h"
#include "Fonts/SlateFontInfo.h"
#include "Layout/Margin.h"
#include "Styling/SlateBrush.h"

class UBorder;
class UComboBoxString;
class UEditableTextBox;
class UHorizontalBox;
class UHorizontalBoxSlot;
class UProgressBar;
class USizeBox;
class USlider;
class UTextBlock;
class UTexture2D;
class UVerticalBox;
class UVerticalBoxSlot;
class UWidget;
class UWidgetTree;

/**
 * The one place the redesigned menu's look is defined.
 *
 * Screens are built in C++ rather than in widget blueprints, so a shared
 * vocabulary has to live somewhere a compiler can see. Everything visual — the
 * palette, the three type faces, the panel and key-cap primitives — is here, and
 * screens are expected to compose from it rather than invent their own colours.
 *
 * The two display faces come from the engine's own font set (Slate/Fonts), so
 * nothing has to be imported to build a screen: "BoldCondensed" is the
 * squashed grotesque the mockups use for headings, "Mono" the label/telemetry
 * face. Swapping in a licensed font later is a change to Font::Display alone.
 */
namespace ApexUI
{
	/**
	 * Dark surfaces, one accent, one "live" green. Anything that needs a colour
	 * not in this list is a design question, not an implementation one.
	 */
	namespace Palette
	{
		/** Page behind everything. */
		inline const FLinearColor Background   = FLinearColor::FromSRGBColor(FColor(0x0E, 0x0E, 0x10));
		/** Cards, rail items, tiles. */
		inline const FLinearColor Surface      = FLinearColor::FromSRGBColor(FColor(0x18, 0x18, 0x1B));
		/** Surface under the cursor or the focus ring. */
		inline const FLinearColor SurfaceHover = FLinearColor::FromSRGBColor(FColor(0x23, 0x23, 0x27));
		/** Hairline around cards and the bars top and bottom. */
		inline const FLinearColor Border       = FLinearColor::FromSRGBColor(FColor(0x2A, 0x2A, 0x2F));

		/** The single accent: primary actions, selected state, personal bests. */
		inline const FLinearColor Accent       = FLinearColor::FromSRGBColor(FColor(0xD9, 0xA3, 0x00));
		/** Text drawn on top of Accent. */
		inline const FLinearColor OnAccent     = FLinearColor::FromSRGBColor(FColor(0x14, 0x14, 0x14));

		inline const FLinearColor TextPrimary  = FLinearColor::FromSRGBColor(FColor(0xF2, 0xF2, 0xF2));
		inline const FLinearColor TextSecondary= FLinearColor::FromSRGBColor(FColor(0xA8, 0xA8, 0xAE));
		inline const FLinearColor TextMuted    = FLinearColor::FromSRGBColor(FColor(0x6E, 0x6E, 0x76));
		/** Disabled rows — dimmer than muted so "locked" reads before the badge does. */
		inline const FLinearColor TextDisabled = FLinearColor::FromSRGBColor(FColor(0x4C, 0x4C, 0x53));

		/** Connected, ready, sessions that are live. */
		inline const FLinearColor Live         = FLinearColor::FromSRGBColor(FColor(0x3F, 0xC4, 0x6B));
		inline const FLinearColor Error        = FLinearColor::FromSRGBColor(FColor(0xE0, 0x5A, 0x4E));
	}

	namespace Font
	{
		/** Headings and button labels. Condensed, always uppercase in the design. */
		FSlateFontInfo Display(float Size, int32 Tracking = 0);
		/** Running text: names, descriptions, values. */
		FSlateFontInfo Body(float Size, bool bBold = false);
		/** Labels, numbers, key caps — anything that should sit on a grid. */
		FSlateFontInfo Mono(float Size, int32 Tracking = 0);
	}

	/** Standard gutters, so screens line up with each other without magic numbers. */
	namespace Metrics
	{
		inline constexpr float TopBarHeight    = 64.0f;
		inline constexpr float HintBarHeight   = 56.0f;
		/** Left/right page margin for screen content. */
		inline constexpr float PageGutter      = 56.0f;
		/** Width of the right-hand rail on the main menu. */
		inline constexpr float RailWidth       = 460.0f;
		/** Height of a rail item / list row. */
		inline constexpr float RowHeight       = 62.0f;
	}

	// --- Primitives -----------------------------------------------------------

	UTextBlock* MakeText(UWidgetTree& Tree, const FString& Text, const FSlateFontInfo& Font, const FLinearColor& Colour);

	/** Uppercase mono caption, letter-spaced — the "ELSEWHERE"/"CAR" style labels. */
	UTextBlock* MakeLabel(UWidgetTree& Tree, const FString& Text, const FLinearColor& Colour = Palette::TextMuted);

	/**
	 * A filled rectangle with an optional hairline outline.
	 *
	 * Outlines need the rounded-box draw type even at radius 0, because that is
	 * the only brush mode Slate strokes.
	 */
	FSlateBrush MakeBrush(
		const FLinearColor& Fill,
		const FLinearColor& Outline = FLinearColor::Transparent,
		float OutlineWidth = 0.0f,
		float CornerRadius = 0.0f);

	UBorder* MakePanel(UWidgetTree& Tree, UWidget* Content, const FMargin& Padding, const FSlateBrush& Brush);

	/** Wraps Content in a size box. Pass a negative extent to leave that axis free. */
	USizeBox* MakeSized(UWidgetTree& Tree, UWidget* Content, float Width, float Height);

	/**
	 * The bordered `ENTER` / `ESC` chips used to advertise a shortcut. The
	 * colours are parameters because a chip on an accent-filled button has to
	 * invert to stay legible.
	 */
	UWidget* MakeKeyCap(
		UWidgetTree& Tree,
		const FString& Key,
		const FLinearColor& TextColour = Palette::TextSecondary,
		const FLinearColor& OutlineColour = Palette::Border);

	/**
	 * Small round status light. OutDot receives the border to recolour later —
	 * status lights nearly always change colour after they are built.
	 */
	UWidget* MakeDot(UWidgetTree& Tree, const FLinearColor& Colour, float Diameter = 9.0f, UBorder** OutDot = nullptr);

	void SetDotColour(UBorder* Dot, const FLinearColor& Colour, float Diameter = 9.0f);

	/** Hairline separator: a full-width rule, or a full-height one. */
	UWidget* MakeDivider(UWidgetTree& Tree, bool bVertical = false);

	/**
	 * The bar every sub-screen wears: a back control, a rule, the screen's name,
	 * and whatever the screen wants on the right (search, filters, status).
	 */
	UWidget* MakeScreenHeader(UWidgetTree& Tree, UWidget* BackControl, const FString& Title, UWidget* RightContent = nullptr);

	/** Label-over-value box, as used in the detail and results panels. */
	UWidget* MakeStat(UWidgetTree& Tree, const FString& Label, UTextBlock*& OutValue, float ValueSize = 23.0f);

	/**
	 * A stat with a bar under it, for values that only mean something next to
	 * their peers — set the bar from the value's share of the fleet's maximum.
	 */
	UWidget* MakeStatBar(
		UWidgetTree& Tree,
		const FString& Label,
		UTextBlock*& OutValue,
		UProgressBar*& OutBar,
		float ValueSize = 27.0f);

	/** Dark search field. Bind OnTextChanged on the returned box. */
	UEditableTextBox* MakeSearchBox(UWidgetTree& Tree, const FString& Hint);

	/**
	 * A labelled slider with a filled track: label left, value right, bar under.
	 *
	 * Slate's slider draws a bar and a thumb but nothing between them, so the
	 * fill is a progress bar sitting behind a slider with a transparent bar.
	 * The caller keeps the two in step in its OnValueChanged handler.
	 */
	UWidget* MakeSliderRow(
		UWidgetTree& Tree,
		const FString& Label,
		UTextBlock*& OutValue,
		UTextBlock*& OutSuffix,
		USlider*& OutSlider,
		UProgressBar*& OutFill);

	/**
	 * Stand-in for art that does not exist: a flat panel with a corner caption.
	 * Preferred over an empty gap, which reads as a bug rather than a gap.
	 */
	UWidget* MakeArtPlaceholder(UWidgetTree& Tree, const FString& Caption);

	/** Preview image at a fixed size, falling back to the placeholder when null. */
	UWidget* MakePreview(
		UWidgetTree& Tree,
		UTexture2D* Texture,
		const FString& PlaceholderCaption,
		float Width,
		float Height);

	/**
	 * Dark dropdown for the settings grid. Bind OnSelectionChanged on the result.
	 *
	 * A combo box rather than another segmented control because these lists are
	 * open-ended — a machine can offer a dozen resolutions, and a dozen pills
	 * would not fit on the row.
	 */
	UComboBoxString* MakeDropdown(UWidgetTree& Tree, const TArray<FString>& Options, int32 SelectedIndex);

	/**
	 * A bare slider with a filled track and no label furniture.
	 *
	 * MakeSliderRow owns its own label and value; the settings grid needs the
	 * track on its own so the label can sit in the row's left-hand cell with
	 * everything else.
	 */
	UWidget* MakeSliderTrack(UWidgetTree& Tree, USlider*& OutSlider, UProgressBar*& OutFill, float Height = 22.0f);

	/**
	 * The floating panel a modal is built on: surface fill, hairline, a little
	 * lift from a darker outer edge.
	 */
	UBorder* MakeModalCard(UWidgetTree& Tree, UWidget* Content, const FMargin& Padding = FMargin(0.0f));

	/**
	 * A full-screen scrim.
	 *
	 * Blurred rather than merely dimmed: a pause menu over a static frame of
	 * the race has to separate itself from a scene that is already busy, and
	 * dimming alone leaves the track legible enough to compete with the menu.
	 */
	UWidget* MakeScrim(UWidgetTree& Tree, UWidget* Content, float BlurStrength = 8.0f);

	/**
	 * One HUD readout: a mono caption over a value.
	 *
	 * Separate from MakeStat because the HUD's are denser, keep the caption
	 * tighter to the value and take their own fill — the race state block reads
	 * as one strip of tiles rather than as a row of cards.
	 */
	UWidget* MakeHudTile(
		UWidgetTree& Tree,
		const FString& Label,
		UTextBlock*& OutValue,
		float ValueSize = 30.0f,
		const FLinearColor& Fill = Palette::Surface,
		const FLinearColor& ValueColour = Palette::TextPrimary);

	/** One "[KEY] ACTION" pair in the footer. */
	struct FKeyHint
	{
		FString Key;
		FString Action;
	};

	/** The footer strip: hints packed left, hints packed right. */
	UWidget* MakeKeyHintBar(UWidgetTree& Tree, const TArray<FKeyHint>& Left, const TArray<FKeyHint>& Right);

	// --- Slot sugar -----------------------------------------------------------
	//
	// Building a tree in C++ is mostly slot configuration; these keep that to one
	// line per child so the shape of a screen stays readable.

	UHorizontalBoxSlot* AddH(
		UHorizontalBox* Box,
		UWidget* Child,
		const FMargin& Padding = FMargin(),
		EVerticalAlignment VAlign = VAlign_Center,
		float FillSize = 0.0f);

	UVerticalBoxSlot* AddV(
		UVerticalBox* Box,
		UWidget* Child,
		const FMargin& Padding = FMargin(),
		EHorizontalAlignment HAlign = HAlign_Fill,
		float FillSize = 0.0f);
}
