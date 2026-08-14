#include "UI/ApexButtonWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "UI/ApexUIStyle.h"

namespace
{
	FLinearColor Lighten(const FLinearColor& Colour, float Amount)
	{
		FLinearColor Result = Colour * (1.0f + Amount);
		Result.A = Colour.A;
		return Result;
	}
}

UApexButtonWidget::UApexButtonWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// A user widget is SelfHitTestInvisible by default, which would send every
	// click straight through to whatever is behind the button.
	SetVisibility(ESlateVisibility::Visible);
	SetIsFocusable(true);
}

void UApexButtonWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	ContentRow = WidgetTree->ConstructWidget<UHorizontalBox>();

	// Full-height stripe on the left edge. Built once and hidden, because a
	// widget appearing and disappearing from the row would shift the label.
	UBorder* Stripe = ApexUI::MakePanel(*WidgetTree, nullptr, FMargin(), ApexUI::MakeBrush(ApexUI::Palette::Accent));
	AccentBar = ApexUI::MakeSized(*WidgetTree, Stripe, 3.0f, -1.0f);
	AccentBar->SetVisibility(ESlateVisibility::Hidden);
	ApexUI::AddH(ContentRow, AccentBar, FMargin(), VAlign_Fill);

	LabelText = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Display(20.0f), ApexUI::Palette::TextPrimary);
	BadgeText = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Mono(10.0f, 120), ApexUI::Palette::TextMuted);

	Background = ApexUI::MakePanel(*WidgetTree, ContentRow, FMargin(), ApexUI::MakeBrush(ApexUI::Palette::Surface));
	// Nothing inside a button may spill over its neighbour, however long a label
	// or badge turns out to be.
	Background->SetClipping(EWidgetClipping::ClipToBounds);
	SizeBox = ApexUI::MakeSized(*WidgetTree, Background, -1.0f, ApexUI::Metrics::RowHeight);

	WidgetTree->RootWidget = SizeBox;

	ApplyState();
}

void UApexButtonWidget::Setup(const FApexButtonSpec& InSpec)
{
	Spec = InSpec;

	if (!ContentRow)
	{
		// Built before initialisation; NativeOnInitialized picks the spec back up.
		return;
	}

	// The row is rebuilt rather than patched: whether there is a badge or a key
	// cap changes the slot layout, not just the text.
	ContentRow->ClearChildren();
	ApexUI::AddH(ContentRow, AccentBar, FMargin(), VAlign_Fill);

	float SidePadding = 18.0f;
	if (Spec.Variant == EApexButtonVariant::Primary)
	{
		SidePadding = 26.0f;
	}
	else if (Spec.Variant == EApexButtonVariant::Bare)
	{
		// A link sits flush with the text around it.
		SidePadding = 0.0f;
	}

	if (Spec.bKeyCapLeading && !Spec.KeyCap.IsEmpty())
	{
		ApexUI::AddH(
			ContentRow,
			ApexUI::MakeKeyCap(*WidgetTree, Spec.KeyCap),
			FMargin(SidePadding, 0.0f, 0.0f, 0.0f));
	}

	// A spec line under the label turns the label cell into a stack.
	UWidget* LabelCell = LabelText;
	if (!Spec.SubLabel.IsEmpty())
	{
		UVerticalBox* Stack = WidgetTree->ConstructWidget<UVerticalBox>();
		ApexUI::AddV(Stack, LabelText);
		SubLabelText = ApexUI::MakeText(
			*WidgetTree,
			Spec.SubLabel.ToUpper(),
			ApexUI::Font::Mono(10.0f, 60),
			ApexUI::Palette::TextMuted);
		ApexUI::AddV(Stack, SubLabelText, FMargin(0.0f, 5.0f, 0.0f, 0.0f));
		LabelCell = Stack;
	}

	// The label cell always takes the slack rather than sitting at its desired
	// width next to a filler: a long label then eats into its own cell instead
	// of pushing the badge out past the button's edge.
	ApexUI::AddH(
		ContentRow,
		LabelCell,
		FMargin(Spec.bKeyCapLeading && !Spec.KeyCap.IsEmpty() ? 10.0f : SidePadding, 0.0f, 0.0f, 0.0f),
		VAlign_Center,
		1.0f);

	LabelText->SetJustification(Spec.bCentreLabel ? ETextJustify::Center : ETextJustify::Left);
	if (SubLabelText)
	{
		SubLabelText->SetJustification(Spec.bCentreLabel ? ETextJustify::Center : ETextJustify::Left);
	}

	if (!Spec.Badge.IsEmpty())
	{
		ApexUI::AddH(ContentRow, BadgeText, FMargin(12.0f, 0.0f, 0.0f, 0.0f));
	}

	if (!Spec.KeyCap.IsEmpty() && !Spec.bKeyCapLeading)
	{
		// On the accent fill the usual grey-on-dark chip disappears, so it flips
		// to the label's own colour with a translucent outline.
		const bool bOnAccent = Spec.Variant == EApexButtonVariant::Primary;
		FLinearColor ChipOutline = ApexUI::Palette::OnAccent;
		ChipOutline.A = 0.45f;

		ApexUI::AddH(
			ContentRow,
			ApexUI::MakeKeyCap(
				*WidgetTree,
				Spec.KeyCap,
				bOnAccent ? ApexUI::Palette::OnAccent : ApexUI::Palette::TextSecondary,
				bOnAccent ? ChipOutline : ApexUI::Palette::Border),
			FMargin(14.0f, 0.0f, 0.0f, 0.0f));
	}

	ApexUI::AddH(ContentRow, WidgetTree->ConstructWidget<UHorizontalBox>(), FMargin(SidePadding, 0.0f, 0.0f, 0.0f));

	if (SizeBox)
	{
		float DefaultHeight = ApexUI::Metrics::RowHeight;
		if (Spec.Variant == EApexButtonVariant::Primary)
		{
			DefaultHeight = 78.0f;
		}
		else if (Spec.Variant == EApexButtonVariant::Bare)
		{
			DefaultHeight = 32.0f;
		}
		SizeBox->SetHeightOverride(Spec.Height > 0.0f ? Spec.Height : DefaultHeight);
	}

	ApplyState();
}

void UApexButtonWidget::SetBadge(const FString& InBadge, const FLinearColor& InColour)
{
	const bool bPresenceChanged = Spec.Badge.IsEmpty() != InBadge.IsEmpty();
	Spec.Badge = InBadge;
	Spec.BadgeColour = InColour;

	if (bPresenceChanged)
	{
		// Adding or removing the badge changes the row's slots.
		Setup(Spec);
		return;
	}

	ApplyState();
}

void UApexButtonWidget::SetLabel(const FString& InLabel)
{
	Spec.Label = InLabel;
	ApplyState();
}

void UApexButtonWidget::SetSelected(bool bInSelected)
{
	if (bSelected == bInSelected)
	{
		return;
	}
	bSelected = bInSelected;
	ApplyState();
}

void UApexButtonWidget::ApplyState()
{
	if (!Background || !LabelText || !BadgeText)
	{
		return;
	}

	const bool bHighlighted = (bHovered || bFocused) && IsInteractive();

	FLinearColor Fill = ApexUI::Palette::Surface;
	FLinearColor Outline = ApexUI::Palette::Border;
	FLinearColor LabelColour = ApexUI::Palette::TextPrimary;
	float OutlineWidth = 1.0f;

	switch (Spec.Variant)
	{
	case EApexButtonVariant::Primary:
		Fill = bHighlighted ? Lighten(ApexUI::Palette::Accent, 0.18f) : ApexUI::Palette::Accent;
		LabelColour = ApexUI::Palette::OnAccent;
		OutlineWidth = 0.0f;
		break;

	case EApexButtonVariant::Panel:
		Fill = bHighlighted ? ApexUI::Palette::SurfaceHover : ApexUI::Palette::Surface;
		break;

	case EApexButtonVariant::Ghost:
		Fill = bHighlighted ? ApexUI::Palette::Surface : FLinearColor::Transparent;
		break;

	case EApexButtonVariant::Locked:
		Fill = ApexUI::Palette::Surface * 0.55f;
		Fill.A = 1.0f;
		Outline = ApexUI::Palette::Border * 0.7f;
		Outline.A = 1.0f;
		LabelColour = ApexUI::Palette::TextDisabled;
		break;

	case EApexButtonVariant::Bare:
		Fill = FLinearColor::Transparent;
		OutlineWidth = 0.0f;
		LabelColour = bHighlighted ? ApexUI::Palette::TextPrimary : ApexUI::Palette::TextSecondary;
		break;
	}

	if (Spec.LabelColour.A > 0.0f)
	{
		// An explicit colour still brightens on hover, so a yellow link reads as
		// interactive rather than as decoration.
		LabelColour = bHighlighted ? Lighten(Spec.LabelColour, 0.25f) : Spec.LabelColour;
	}

	if (bSelected)
	{
		Outline = ApexUI::Palette::Accent;
		OutlineWidth = 2.0f;
	}

	Background->SetBrush(ApexUI::MakeBrush(Fill, Outline, OutlineWidth));

	const bool bIsPrimary = Spec.Variant == EApexButtonVariant::Primary;
	const float LabelSize = Spec.LabelSize > 0.0f ? Spec.LabelSize : (bIsPrimary ? 26.0f : 20.0f);

	LabelText->SetText(FText::FromString(bIsPrimary ? Spec.Label.ToUpper() : Spec.Label));
	LabelText->SetFont(ApexUI::Font::Display(LabelSize, bIsPrimary ? 30 : 0));
	LabelText->SetColorAndOpacity(FSlateColor(LabelColour));

	BadgeText->SetText(FText::FromString(Spec.Badge.ToUpper()));
	BadgeText->SetColorAndOpacity(FSlateColor(
		Spec.Variant == EApexButtonVariant::Locked ? ApexUI::Palette::TextDisabled : Spec.BadgeColour));

	if (AccentBar)
	{
		// Focus, not hover: the stripe marks where the keyboard is, and a mouse
		// moving across the rail should not make it look like the selection moved.
		AccentBar->SetVisibility(
			bFocused && Spec.Variant == EApexButtonVariant::Panel
				? ESlateVisibility::Visible
				: ESlateVisibility::Hidden);
	}
}

void UApexButtonWidget::Activate()
{
	if (!IsInteractive())
	{
		return;
	}
	OnActivated.Broadcast(this);
}

FReply UApexButtonWidget::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (!IsInteractive() || InMouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return FReply::Unhandled();
	}

	// Take focus on click so the keyboard carries on from where the mouse left.
	SetKeyboardFocus();
	Activate();
	return FReply::Handled();
}

void UApexButtonWidget::NativeOnMouseEnter(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	Super::NativeOnMouseEnter(InGeometry, InMouseEvent);
	bHovered = true;
	ApplyState();
}

void UApexButtonWidget::NativeOnMouseLeave(const FPointerEvent& InMouseEvent)
{
	Super::NativeOnMouseLeave(InMouseEvent);
	bHovered = false;
	ApplyState();
}

void UApexButtonWidget::NativeOnAddedToFocusPath(const FFocusEvent& InFocusEvent)
{
	Super::NativeOnAddedToFocusPath(InFocusEvent);
	bFocused = true;
	ApplyState();
}

void UApexButtonWidget::NativeOnRemovedFromFocusPath(const FFocusEvent& InFocusEvent)
{
	Super::NativeOnRemovedFromFocusPath(InFocusEvent);
	bFocused = false;
	ApplyState();
}

FReply UApexButtonWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();
	if (IsInteractive()
		&& (Key == EKeys::Enter || Key == EKeys::SpaceBar || Key == EKeys::Virtual_Accept))
	{
		Activate();
		return FReply::Handled();
	}

	// Everything else — arrows, Tab, Escape — belongs to the screen, which is
	// further up the focus path.
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}
