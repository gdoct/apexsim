#include "UI/ApexUIStyle.h"

#include "Blueprint/WidgetTree.h"
#include "Components/BackgroundBlur.h"
#include "Components/Border.h"
#include "Components/ComboBoxString.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/ProgressBar.h"
#include "Components/ScaleBox.h"
#include "Components/SizeBox.h"
#include "Components/Slider.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Styling/CoreStyle.h"

namespace ApexUI
{
	FSlateFontInfo Font::Display(float Size, int32 Tracking)
	{
		// "BoldCondensed" is Roboto-BoldCondensed, shipped with Slate — the
		// closest thing in-engine to the compressed grotesque in the mockups.
		FSlateFontInfo Info = FCoreStyle::GetDefaultFontStyle(TEXT("BoldCondensed"), Size);
		Info.LetterSpacing = Tracking;
		return Info;
	}

	FSlateFontInfo Font::Body(float Size, bool bBold)
	{
		return FCoreStyle::GetDefaultFontStyle(bBold ? TEXT("Bold") : TEXT("Regular"), Size);
	}

	FSlateFontInfo Font::Mono(float Size, int32 Tracking)
	{
		FSlateFontInfo Info = FCoreStyle::GetDefaultFontStyle(TEXT("Mono"), Size);
		Info.LetterSpacing = Tracking;
		return Info;
	}

	UTextBlock* MakeText(UWidgetTree& Tree, const FString& Text, const FSlateFontInfo& Font, const FLinearColor& Colour)
	{
		UTextBlock* Block = Tree.ConstructWidget<UTextBlock>();
		Block->SetText(FText::FromString(Text));
		Block->SetFont(Font);
		Block->SetColorAndOpacity(FSlateColor(Colour));
		return Block;
	}

	UTextBlock* MakeLabel(UWidgetTree& Tree, const FString& Text, const FLinearColor& Colour)
	{
		return MakeText(Tree, Text.ToUpper(), Font::Mono(10.0f, 160), Colour);
	}

	FSlateBrush MakeBrush(const FLinearColor& Fill, const FLinearColor& Outline, float OutlineWidth, float CornerRadius)
	{
		FSlateBrush Brush;
		Brush.TintColor = FSlateColor(Fill);

		if (OutlineWidth > 0.0f || CornerRadius > 0.0f)
		{
			// Rounded-box is the only draw type Slate strokes an outline on, so
			// square-cornered outlined panels are rounded boxes at radius 0.
			Brush.DrawAs = ESlateBrushDrawType::RoundedBox;
			Brush.OutlineSettings = FSlateBrushOutlineSettings(
				FVector4(CornerRadius, CornerRadius, CornerRadius, CornerRadius),
				FSlateColor(Outline),
				OutlineWidth);
			Brush.OutlineSettings.RoundingType = ESlateBrushRoundingType::FixedRadius;
		}
		else
		{
			Brush.DrawAs = ESlateBrushDrawType::Box;
		}

		return Brush;
	}

	UBorder* MakePanel(UWidgetTree& Tree, UWidget* Content, const FMargin& Padding, const FSlateBrush& Brush)
	{
		UBorder* Panel = Tree.ConstructWidget<UBorder>();
		Panel->SetBrush(Brush);
		Panel->SetPadding(Padding);
		Panel->SetHorizontalAlignment(HAlign_Fill);
		Panel->SetVerticalAlignment(VAlign_Fill);
		if (Content)
		{
			Panel->SetContent(Content);
		}
		return Panel;
	}

	USizeBox* MakeSized(UWidgetTree& Tree, UWidget* Content, float Width, float Height)
	{
		USizeBox* Box = Tree.ConstructWidget<USizeBox>();
		if (Width >= 0.0f)
		{
			Box->SetWidthOverride(Width);
		}
		if (Height >= 0.0f)
		{
			Box->SetHeightOverride(Height);
		}
		if (Content)
		{
			Box->SetContent(Content);
		}
		return Box;
	}

	UWidget* MakeKeyCap(UWidgetTree& Tree, const FString& Key, const FLinearColor& TextColour, const FLinearColor& OutlineColour)
	{
		UTextBlock* Text = MakeText(Tree, Key.ToUpper(), Font::Mono(9.0f, 80), TextColour);
		return MakePanel(
			Tree,
			Text,
			FMargin(7.0f, 3.0f),
			MakeBrush(FLinearColor::Transparent, OutlineColour, 1.0f, 2.0f));
	}

	UWidget* MakeDot(UWidgetTree& Tree, const FLinearColor& Colour, float Diameter, UBorder** OutDot)
	{
		UBorder* Dot = Tree.ConstructWidget<UBorder>();
		SetDotColour(Dot, Colour, Diameter);

		if (OutDot)
		{
			*OutDot = Dot;
		}
		return MakeSized(Tree, Dot, Diameter, Diameter);
	}

	void SetDotColour(UBorder* Dot, const FLinearColor& Colour, float Diameter)
	{
		if (!Dot)
		{
			return;
		}

		FSlateBrush Brush = MakeBrush(Colour, FLinearColor::Transparent, 0.0f, Diameter * 0.5f);
		Brush.OutlineSettings.RoundingType = ESlateBrushRoundingType::HalfHeightRadius;
		Dot->SetBrush(Brush);
	}

	UWidget* MakeDivider(UWidgetTree& Tree, bool bVertical)
	{
		UBorder* Rule = MakePanel(Tree, nullptr, FMargin(), MakeBrush(Palette::Border));
		return bVertical
			? MakeSized(Tree, Rule, 1.0f, -1.0f)
			: MakeSized(Tree, Rule, -1.0f, 1.0f);
	}

	UWidget* MakeScreenHeader(UWidgetTree& Tree, UWidget* BackControl, const FString& Title, UWidget* RightContent)
	{
		UHorizontalBox* Row = Tree.ConstructWidget<UHorizontalBox>();

		if (BackControl)
		{
			AddH(Row, BackControl);
			AddH(Row, MakeSized(Tree, MakeDivider(Tree, true), 1.0f, 22.0f), FMargin(18.0f, 0.0f));
		}

		AddH(Row, MakeText(Tree, Title, Font::Display(22.0f), Palette::TextPrimary), FMargin(6.0f, 0.0f, 0.0f, 0.0f));
		AddH(Row, Tree.ConstructWidget<UHorizontalBox>(), FMargin(), VAlign_Center, 1.0f);

		if (RightContent)
		{
			AddH(Row, RightContent);
		}

		UVerticalBox* Stack = Tree.ConstructWidget<UVerticalBox>();

		UBorder* Bar = MakePanel(Tree, Row, FMargin(Metrics::PageGutter, 0.0f), MakeBrush(Palette::Background));
		Bar->SetVerticalAlignment(VAlign_Center);
		AddV(Stack, MakeSized(Tree, Bar, -1.0f, Metrics::TopBarHeight));
		AddV(Stack, MakeDivider(Tree));

		return Stack;
	}

	UWidget* MakeStat(UWidgetTree& Tree, const FString& Label, UTextBlock*& OutValue, float ValueSize)
	{
		UVerticalBox* Box = Tree.ConstructWidget<UVerticalBox>();
		AddV(Box, MakeLabel(Tree, Label));

		OutValue = MakeText(Tree, FString(), Font::Display(ValueSize), Palette::TextPrimary);
		AddV(Box, OutValue, FMargin(0.0f, 7.0f, 0.0f, 0.0f));

		return MakePanel(Tree, Box, FMargin(16.0f, 12.0f), MakeBrush(Palette::Surface, Palette::Border, 1.0f));
	}

	UWidget* MakeStatBar(UWidgetTree& Tree, const FString& Label, UTextBlock*& OutValue, UProgressBar*& OutBar, float ValueSize)
	{
		UVerticalBox* Box = Tree.ConstructWidget<UVerticalBox>();
		AddV(Box, MakeLabel(Tree, Label));

		OutValue = MakeText(Tree, FString(), Font::Display(ValueSize), Palette::TextPrimary);
		AddV(Box, OutValue, FMargin(0.0f, 6.0f, 0.0f, 0.0f));

		OutBar = Tree.ConstructWidget<UProgressBar>();
		FProgressBarStyle BarStyle = OutBar->WidgetStyle;
		BarStyle.SetBackgroundImage(MakeBrush(Palette::Border));
		BarStyle.SetFillImage(MakeBrush(Palette::Accent));
		OutBar->WidgetStyle = BarStyle;
		OutBar->SetPercent(0.0f);
		AddV(Box, MakeSized(Tree, OutBar, -1.0f, 3.0f), FMargin(0.0f, 12.0f, 0.0f, 0.0f));

		return MakePanel(Tree, Box, FMargin(16.0f, 13.0f), MakeBrush(Palette::Surface, Palette::Border, 1.0f));
	}

	UEditableTextBox* MakeSearchBox(UWidgetTree& Tree, const FString& Hint)
	{
		UEditableTextBox* Box = Tree.ConstructWidget<UEditableTextBox>();

		FEditableTextBoxStyle Style = Box->WidgetStyle;
		const FSlateBrush Idle = MakeBrush(Palette::Surface, Palette::Border, 1.0f);
		const FSlateBrush Active = MakeBrush(Palette::SurfaceHover, Palette::Accent, 1.0f);
		Style.SetBackgroundImageNormal(Idle);
		Style.SetBackgroundImageHovered(Idle);
		Style.SetBackgroundImageFocused(Active);
		Style.SetBackgroundImageReadOnly(Idle);
		Style.SetPadding(FMargin(12.0f, 8.0f));
		Style.SetFont(Font::Mono(11.0f));
		Style.SetForegroundColor(FSlateColor(Palette::TextPrimary));

		Box->WidgetStyle = Style;
		Box->SetHintText(FText::FromString(Hint.ToUpper()));
		return Box;
	}

	UWidget* MakeSliderRow(
		UWidgetTree& Tree,
		const FString& Label,
		UTextBlock*& OutValue,
		UTextBlock*& OutSuffix,
		USlider*& OutSlider,
		UProgressBar*& OutFill)
	{
		UHorizontalBox* Head = Tree.ConstructWidget<UHorizontalBox>();
		AddH(Head, MakeLabel(Tree, Label));
		AddH(Head, Tree.ConstructWidget<UHorizontalBox>(), FMargin(), VAlign_Center, 1.0f);

		OutValue = MakeText(Tree, FString(), Font::Display(24.0f), Palette::TextPrimary);
		AddH(Head, OutValue);

		OutSuffix = MakeText(Tree, FString(), Font::Mono(10.0f, 60), Palette::TextMuted);
		AddH(Head, OutSuffix, FMargin(8.0f, 0.0f, 0.0f, 0.0f));

		OutFill = Tree.ConstructWidget<UProgressBar>();
		FProgressBarStyle FillStyle = OutFill->WidgetStyle;
		FillStyle.SetBackgroundImage(MakeBrush(Palette::Border));
		FillStyle.SetFillImage(MakeBrush(Palette::Accent));
		OutFill->WidgetStyle = FillStyle;

		OutSlider = Tree.ConstructWidget<USlider>();
		FSliderStyle SliderStyle = OutSlider->WidgetStyle;
		SliderStyle.SetNormalBarImage(MakeBrush(FLinearColor::Transparent));
		SliderStyle.SetHoveredBarImage(MakeBrush(FLinearColor::Transparent));
		SliderStyle.SetDisabledBarImage(MakeBrush(FLinearColor::Transparent));

		FSlateBrush Thumb = MakeBrush(Palette::TextPrimary);
		Thumb.ImageSize = FVector2D(5.0f, 20.0f);
		SliderStyle.SetNormalThumbImage(Thumb);
		SliderStyle.SetHoveredThumbImage(Thumb);
		SliderStyle.SetDisabledThumbImage(Thumb);
		SliderStyle.SetBarThickness(6.0f);
		OutSlider->WidgetStyle = SliderStyle;

		UOverlay* Track = Tree.ConstructWidget<UOverlay>();
		UOverlaySlot* FillSlot = Track->AddChildToOverlay(MakeSized(Tree, OutFill, -1.0f, 6.0f));
		FillSlot->SetHorizontalAlignment(HAlign_Fill);
		FillSlot->SetVerticalAlignment(VAlign_Center);
		UOverlaySlot* SliderSlot = Track->AddChildToOverlay(OutSlider);
		SliderSlot->SetHorizontalAlignment(HAlign_Fill);
		SliderSlot->SetVerticalAlignment(VAlign_Center);

		UVerticalBox* Box = Tree.ConstructWidget<UVerticalBox>();
		AddV(Box, Head);
		AddV(Box, MakeSized(Tree, Track, -1.0f, 22.0f), FMargin(0.0f, 8.0f, 0.0f, 0.0f));
		return Box;
	}

	UComboBoxString* MakeDropdown(UWidgetTree& Tree, const TArray<FString>& Options, int32 SelectedIndex)
	{
		UComboBoxString* Box = Tree.ConstructWidget<UComboBoxString>();

		FComboBoxStyle ComboStyle = Box->WidgetStyle;
		FComboButtonStyle ButtonStyle = ComboStyle.ComboButtonStyle;
		FButtonStyle Inner = ButtonStyle.ButtonStyle;
		Inner.SetNormal(MakeBrush(Palette::Surface, Palette::Border, 1.0f));
		Inner.SetHovered(MakeBrush(Palette::SurfaceHover, Palette::Border, 1.0f));
		Inner.SetPressed(MakeBrush(Palette::SurfaceHover, Palette::Accent, 1.0f));
		Inner.SetNormalPadding(FMargin(12.0f, 7.0f));
		Inner.SetPressedPadding(FMargin(12.0f, 7.0f));
		ButtonStyle.SetButtonStyle(Inner);
		// The popup is a separate surface from the closed box, and left at the
		// engine default it is a pale grey panel in the middle of a dark screen.
		ButtonStyle.SetMenuBorderBrush(MakeBrush(Palette::Surface, Palette::Border, 1.0f));
		ButtonStyle.SetMenuBorderPadding(FMargin(1.0f));
		ComboStyle.SetComboButtonStyle(ButtonStyle);
		Box->WidgetStyle = ComboStyle;

		FTableRowStyle RowStyle = Box->ItemStyle;
		RowStyle.SetEvenRowBackgroundBrush(MakeBrush(Palette::Surface));
		RowStyle.SetOddRowBackgroundBrush(MakeBrush(Palette::Surface));
		RowStyle.SetEvenRowBackgroundHoveredBrush(MakeBrush(Palette::SurfaceHover));
		RowStyle.SetOddRowBackgroundHoveredBrush(MakeBrush(Palette::SurfaceHover));
		RowStyle.SetActiveBrush(MakeBrush(Palette::Accent));
		RowStyle.SetActiveHoveredBrush(MakeBrush(Palette::Accent));
		RowStyle.SetSelectorFocusedBrush(MakeBrush(Palette::Accent));
		RowStyle.SetTextColor(FSlateColor(Palette::TextPrimary));
		RowStyle.SetSelectedTextColor(FSlateColor(Palette::OnAccent));
		Box->ItemStyle = RowStyle;

		// Font and ForegroundColor are construction-time properties with no public
		// setter — InitFont is protected — so they are assigned directly. The
		// deprecation warning is about runtime mutation, which is not what this
		// is: the box has not been built yet.
		PRAGMA_DISABLE_DEPRECATION_WARNINGS
		Box->Font = Font::Mono(11.0f, 40);
		Box->ForegroundColor = FSlateColor(Palette::TextPrimary);
		PRAGMA_ENABLE_DEPRECATION_WARNINGS
		Box->SetContentPadding(FMargin(12.0f, 7.0f));

		for (const FString& Option : Options)
		{
			Box->AddOption(Option);
		}
		if (Options.IsValidIndex(SelectedIndex))
		{
			Box->SetSelectedIndex(SelectedIndex);
		}

		return Box;
	}

	UWidget* MakeSliderTrack(UWidgetTree& Tree, USlider*& OutSlider, UProgressBar*& OutFill, float Height)
	{
		OutFill = Tree.ConstructWidget<UProgressBar>();
		FProgressBarStyle FillStyle = OutFill->WidgetStyle;
		FillStyle.SetBackgroundImage(MakeBrush(Palette::Border));
		FillStyle.SetFillImage(MakeBrush(Palette::Accent));
		OutFill->WidgetStyle = FillStyle;

		OutSlider = Tree.ConstructWidget<USlider>();
		FSliderStyle SliderStyle = OutSlider->WidgetStyle;
		// Same trick as MakeSliderRow: Slate draws a bar and a thumb but nothing
		// between them, so the fill is a progress bar behind a transparent slider.
		SliderStyle.SetNormalBarImage(MakeBrush(FLinearColor::Transparent));
		SliderStyle.SetHoveredBarImage(MakeBrush(FLinearColor::Transparent));
		SliderStyle.SetDisabledBarImage(MakeBrush(FLinearColor::Transparent));

		FSlateBrush Thumb = MakeBrush(Palette::TextPrimary);
		Thumb.ImageSize = FVector2D(4.0f, 16.0f);
		SliderStyle.SetNormalThumbImage(Thumb);
		SliderStyle.SetHoveredThumbImage(Thumb);
		SliderStyle.SetDisabledThumbImage(Thumb);
		SliderStyle.SetBarThickness(4.0f);
		OutSlider->WidgetStyle = SliderStyle;

		UOverlay* Track = Tree.ConstructWidget<UOverlay>();
		UOverlaySlot* FillSlot = Track->AddChildToOverlay(MakeSized(Tree, OutFill, -1.0f, 4.0f));
		FillSlot->SetHorizontalAlignment(HAlign_Fill);
		FillSlot->SetVerticalAlignment(VAlign_Center);
		UOverlaySlot* SliderSlot = Track->AddChildToOverlay(OutSlider);
		SliderSlot->SetHorizontalAlignment(HAlign_Fill);
		SliderSlot->SetVerticalAlignment(VAlign_Center);

		return MakeSized(Tree, Track, -1.0f, Height);
	}

	UBorder* MakeModalCard(UWidgetTree& Tree, UWidget* Content, const FMargin& Padding)
	{
		return MakePanel(Tree, Content, Padding, MakeBrush(Palette::Surface, Palette::Border, 1.0f));
	}

	UWidget* MakeScrim(UWidgetTree& Tree, UWidget* Content, float BlurStrength)
	{
		UBackgroundBlur* Blur = Tree.ConstructWidget<UBackgroundBlur>();
		Blur->SetBlurStrength(BlurStrength);
		Blur->SetPadding(FMargin(0.0f));
		Blur->SetHorizontalAlignment(HAlign_Fill);
		Blur->SetVerticalAlignment(VAlign_Fill);

		// Blur alone leaves a bright scene bright. The wash underneath the content
		// is what actually buys the contrast the panel needs.
		FLinearColor Wash = Palette::Background;
		Wash.A = 0.55f;

		UOverlay* Stack = Tree.ConstructWidget<UOverlay>();
		UOverlaySlot* WashSlot = Stack->AddChildToOverlay(
			MakePanel(Tree, nullptr, FMargin(), MakeBrush(Wash)));
		WashSlot->SetHorizontalAlignment(HAlign_Fill);
		WashSlot->SetVerticalAlignment(VAlign_Fill);

		if (Content)
		{
			UOverlaySlot* ContentSlot = Stack->AddChildToOverlay(Content);
			ContentSlot->SetHorizontalAlignment(HAlign_Fill);
			ContentSlot->SetVerticalAlignment(VAlign_Fill);
		}

		Blur->SetContent(Stack);
		return Blur;
	}

	UWidget* MakeHudTile(
		UWidgetTree& Tree,
		const FString& Label,
		UTextBlock*& OutValue,
		float ValueSize,
		const FLinearColor& Fill,
		const FLinearColor& ValueColour)
	{
		UVerticalBox* Box = Tree.ConstructWidget<UVerticalBox>();

		// The caption takes its colour from the fill: on the accent tile the usual
		// muted grey is unreadable.
		const bool bOnAccent = Fill.Equals(Palette::Accent, 0.01f);
		FLinearColor CaptionColour = bOnAccent ? Palette::OnAccent : Palette::TextMuted;
		if (bOnAccent)
		{
			CaptionColour.A = 0.75f;
		}
		AddV(Box, MakeLabel(Tree, Label, CaptionColour), FMargin(), HAlign_Center);

		OutValue = MakeText(Tree, FString(), Font::Display(ValueSize), ValueColour);
		AddV(Box, OutValue, FMargin(0.0f, 4.0f, 0.0f, 0.0f), HAlign_Center);

		UBorder* Panel = MakePanel(Tree, Box, FMargin(16.0f, 10.0f), MakeBrush(Fill));
		Panel->SetVerticalAlignment(VAlign_Center);
		return Panel;
	}

	UWidget* MakeArtPlaceholder(UWidgetTree& Tree, const FString& Caption)
	{
		UBorder* Panel = MakePanel(
			Tree,
			MakeLabel(Tree, Caption, Palette::TextDisabled),
			FMargin(12.0f, 10.0f),
			MakeBrush(Palette::SurfaceHover * 0.7f, Palette::Border, 1.0f));
		Panel->SetHorizontalAlignment(HAlign_Left);
		Panel->SetVerticalAlignment(VAlign_Bottom);
		return Panel;
	}

	UWidget* MakePreview(UWidgetTree& Tree, UTexture2D* Texture, const FString& PlaceholderCaption, float Width, float Height)
	{
		if (!Texture)
		{
			return MakeSized(Tree, MakeArtPlaceholder(Tree, PlaceholderCaption), Width, Height);
		}

		// The art is drawn at its own aspect ratio (the generated outlines are
		// 4:3) and letterboxed into the slot, which is rarely 4:3: a card is
		// about 2:1 and the detail panel 1.4:1. Stretching to the slot squashed
		// every circuit differently on every screen. The bars are painted in the
		// art's own background so they do not read as a frame.
		UImage* Image = Tree.ConstructWidget<UImage>();
		Image->SetBrushFromTexture(Texture, true);
		FSlateBrush Brush = Image->GetBrush();
		Brush.DrawAs = ESlateBrushDrawType::Image;
		Image->SetBrush(Brush);

		UScaleBox* Fit = Tree.ConstructWidget<UScaleBox>();
		Fit->SetStretch(EStretch::ScaleToFit);
		Fit->AddChild(Image);

		// Same colour as scripts/generate_track_previews.py's BACKGROUND_COLOR.
		UBorder* Matte = MakePanel(
			Tree,
			Fit,
			FMargin(),
			MakeBrush(FLinearColor::FromSRGBColor(FColor(20, 20, 30))));
		Matte->SetHorizontalAlignment(HAlign_Fill);
		Matte->SetVerticalAlignment(VAlign_Fill);

		return MakeSized(Tree, Matte, Width, Height);
	}

	namespace
	{
		/** "[KEY] ACTION", the unit the footer is built from. */
		UWidget* MakeHintPair(UWidgetTree& Tree, const FKeyHint& Hint)
		{
			UHorizontalBox* Row = Tree.ConstructWidget<UHorizontalBox>();
			AddH(Row, MakeKeyCap(Tree, Hint.Key));
			AddH(Row, MakeText(Tree, Hint.Action.ToUpper(), Font::Mono(9.0f, 120), Palette::TextMuted), FMargin(8.0f, 0.0f, 0.0f, 0.0f));
			return Row;
		}
	}

	UWidget* MakeKeyHintBar(UWidgetTree& Tree, const TArray<FKeyHint>& Left, const TArray<FKeyHint>& Right)
	{
		UHorizontalBox* Row = Tree.ConstructWidget<UHorizontalBox>();

		for (const FKeyHint& Hint : Left)
		{
			AddH(Row, MakeHintPair(Tree, Hint), FMargin(0.0f, 0.0f, 28.0f, 0.0f));
		}

		// Empty filler pushes the right-hand group to the far edge.
		UHorizontalBox* Filler = Tree.ConstructWidget<UHorizontalBox>();
		AddH(Row, Filler, FMargin(), VAlign_Center, 1.0f);

		for (const FKeyHint& Hint : Right)
		{
			AddH(Row, MakeHintPair(Tree, Hint), FMargin(28.0f, 0.0f, 0.0f, 0.0f));
		}

		UBorder* Bar = MakePanel(
			Tree,
			Row,
			FMargin(Metrics::PageGutter, 0.0f),
			MakeBrush(Palette::Background));
		Bar->SetVerticalAlignment(VAlign_Center);

		return MakeSized(Tree, Bar, -1.0f, Metrics::HintBarHeight);
	}

	UHorizontalBoxSlot* AddH(UHorizontalBox* Box, UWidget* Child, const FMargin& Padding, EVerticalAlignment VAlign, float FillSize)
	{
		if (!Box || !Child)
		{
			return nullptr;
		}

		UHorizontalBoxSlot* Slot = Box->AddChildToHorizontalBox(Child);
		Slot->SetPadding(Padding);
		Slot->SetVerticalAlignment(VAlign);
		Slot->SetSize(FillSize > 0.0f
			? FSlateChildSize(ESlateSizeRule::Fill)
			: FSlateChildSize(ESlateSizeRule::Automatic));
		return Slot;
	}

	UVerticalBoxSlot* AddV(UVerticalBox* Box, UWidget* Child, const FMargin& Padding, EHorizontalAlignment HAlign, float FillSize)
	{
		if (!Box || !Child)
		{
			return nullptr;
		}

		UVerticalBoxSlot* Slot = Box->AddChildToVerticalBox(Child);
		Slot->SetPadding(Padding);
		Slot->SetHorizontalAlignment(HAlign);
		Slot->SetSize(FillSize > 0.0f
			? FSlateChildSize(ESlateSizeRule::Fill)
			: FSlateChildSize(ESlateSizeRule::Automatic));
		return Slot;
	}
}
