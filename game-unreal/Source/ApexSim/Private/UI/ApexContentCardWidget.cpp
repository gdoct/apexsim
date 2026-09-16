#include "UI/ApexContentCardWidget.h"

#include "Audio/ApexUiAudioSubsystem.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "UI/ApexNavigation.h"
#include "UI/ApexUIStyle.h"

UApexContentCardWidget::UApexContentCardWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetVisibility(ESlateVisibility::Visible);
	SetIsFocusable(true);
}

void UApexContentCardWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	Body = WidgetTree->ConstructWidget<UVerticalBox>();
	Frame = ApexUI::MakePanel(
		*WidgetTree,
		Body,
		FMargin(),
		ApexUI::MakeBrush(ApexUI::Palette::Surface, ApexUI::Palette::Border, 1.0f));

	UOverlay* Layers = WidgetTree->ConstructWidget<UOverlay>();
	UOverlaySlot* FrameSlot = Layers->AddChildToOverlay(Frame);
	FrameSlot->SetHorizontalAlignment(HAlign_Fill);
	FrameSlot->SetVerticalAlignment(VAlign_Fill);
	SelectedRing = ApexUI::AddFocusRing(*WidgetTree, *Layers, FMargin(ApexUI::Metrics::FocusRingInset));
	SelectedRing->SetBrush(ApexUI::MakeBrush(FLinearColor::Transparent, ApexUI::Palette::Accent, 2.0f));
	FocusRing = ApexUI::AddFocusRing(*WidgetTree, *Layers);

	WidgetTree->RootWidget = Layers;
	ApplyState();
}

void UApexContentCardWidget::Setup(const FApexCardSpec& InSpec)
{
	Spec = InSpec;

	if (!Body)
	{
		return;
	}

	Body->ClearChildren();

	// The art is a fresh widget each time: a card can go from a texture to a
	// placeholder when the catalog has no row for the content.
	ApexUI::AddV(
		Body,
		ApexUI::MakePreview(*WidgetTree, Spec.Preview, Spec.PlaceholderCaption, -1.0f, Spec.PreviewHeight));

	UVerticalBox* TextBlock = WidgetTree->ConstructWidget<UVerticalBox>();

	TitleText = ApexUI::MakeText(*WidgetTree, Spec.Title, ApexUI::Font::Display(19.0f), ApexUI::Palette::TextPrimary);
	// Circuit names run long ("Circuit de la Sarthe (Le Mans 24 Hours)"); wrapping
	// keeps them inside the card instead of over its neighbour.
	TitleText->SetAutoWrapText(true);
	ApexUI::AddV(TextBlock, TitleText);

	MetaText = ApexUI::MakeText(*WidgetTree, Spec.Meta.ToUpper(), ApexUI::Font::Mono(10.0f, 60), ApexUI::Palette::TextMuted);
	ApexUI::AddV(TextBlock, MetaText, FMargin(0.0f, 7.0f, 0.0f, 0.0f));

	FootnoteText = ApexUI::MakeText(*WidgetTree, Spec.Footnote.ToUpper(), ApexUI::Font::Mono(10.0f, 60), Spec.FootnoteColour);
	ApexUI::AddV(TextBlock, FootnoteText, FMargin(0.0f, 5.0f, 0.0f, 0.0f));

	ApexUI::AddV(Body, ApexUI::MakePanel(*WidgetTree, TextBlock, FMargin(15.0f, 13.0f), ApexUI::MakeBrush(FLinearColor::Transparent)));

	ApplyState();
}

void UApexContentCardWidget::SetSelected(bool bInSelected)
{
	if (bSelected == bInSelected)
	{
		return;
	}
	bSelected = bInSelected;
	ApplyState();
}

void UApexContentCardWidget::ApplyState()
{
	if (!Frame)
	{
		return;
	}

	// Selection owns the accent outline, focus the white ring, hover the fill.
	// With the ring on the edge the selected outline steps inside it, so a card
	// can read as both "where the pad is" and "the one chosen".
	const bool bShowFocusRing = bFocused && !bFocusFromPointer;
	const bool bEdgeSelected = bSelected && !bShowFocusRing;
	const FLinearColor Outline = bEdgeSelected ? ApexUI::Palette::Accent : ApexUI::Palette::Border;
	const FLinearColor Fill = (bHovered || bFocused) ? ApexUI::Palette::SurfaceHover : ApexUI::Palette::Surface;

	Frame->SetBrush(ApexUI::MakeBrush(Fill, Outline, bEdgeSelected ? 2.0f : 1.0f));

	if (FocusRing)
	{
		FocusRing->SetVisibility(bShowFocusRing ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Hidden);
	}
	if (SelectedRing)
	{
		SelectedRing->SetVisibility(
			bShowFocusRing && bSelected ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Hidden);
	}
}

FReply UApexContentCardWidget::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (InMouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return FReply::Unhandled();
	}

	bFocusFromPointer = true;
	SetKeyboardFocus();
	ApplyState();
	ApexUiAudio::Play(this, EApexUiSound::Accept);
	OnActivated.Broadcast(this);
	return FReply::Handled();
}

void UApexContentCardWidget::NativeOnMouseEnter(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	Super::NativeOnMouseEnter(InGeometry, InMouseEvent);
	bHovered = true;
	ApplyState();
}

void UApexContentCardWidget::NativeOnMouseLeave(const FPointerEvent& InMouseEvent)
{
	Super::NativeOnMouseLeave(InMouseEvent);
	bHovered = false;
	ApplyState();
}

void UApexContentCardWidget::NativeOnAddedToFocusPath(const FFocusEvent& InFocusEvent)
{
	Super::NativeOnAddedToFocusPath(InFocusEvent);
	bFocused = true;
	bFocusFromPointer = bFocusFromPointer || InFocusEvent.GetCause() == EFocusCause::Mouse;
	ApplyState();
}

void UApexContentCardWidget::NativeOnRemovedFromFocusPath(const FFocusEvent& InFocusEvent)
{
	Super::NativeOnRemovedFromFocusPath(InFocusEvent);
	bFocused = false;
	bFocusFromPointer = false;
	ApplyState();
}

FReply UApexContentCardWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	if (bFocusFromPointer)
	{
		bFocusFromPointer = false;
		ApplyState();
	}

	if (ApexNav::IsAccept(InKeyEvent))
	{
		ApexUiAudio::Play(this, EApexUiSound::Accept);
		OnActivated.Broadcast(this);
		return FReply::Handled();
	}

	// Arrows belong to the grid, which is the screen.
	const EUINavigation Direction = ApexNav::DirectionFromKey(InKeyEvent);
	if (Direction != EUINavigation::Invalid)
	{
		return ApexNav::RouteFromLeaf(this, Direction,
			InKeyEvent.GetKey().IsGamepadKey() ? ENavigationGenesis::Controller : ENavigationGenesis::Keyboard);
	}

	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

FReply UApexContentCardWidget::NativeOnAnalogValueChanged(const FGeometry& InGeometry, const FAnalogInputEvent& InAnalogEvent)
{
	if (!ApexNav::IsAnalogNavigationKey(InAnalogEvent.GetKey()))
	{
		return Super::NativeOnAnalogValueChanged(InGeometry, InAnalogEvent);
	}

	const EUINavigation Direction = ApexNav::DirectionFromAnalog(InAnalogEvent);
	if (Direction == EUINavigation::Invalid)
	{
		return FReply::Handled();
	}
	return ApexNav::RouteFromLeaf(this, Direction, ENavigationGenesis::Controller);
}
