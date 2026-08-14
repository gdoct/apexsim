#include "UI/ApexContentCardWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
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

	WidgetTree->RootWidget = Frame;
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

	// Selection owns the outline, focus and hover own the fill. That way a card
	// stays visibly "the one being described" while the keyboard moves over it.
	const FLinearColor Outline = bSelected ? ApexUI::Palette::Accent
		: (bFocused ? ApexUI::Palette::TextMuted : ApexUI::Palette::Border);
	const FLinearColor Fill = (bHovered || bFocused) ? ApexUI::Palette::SurfaceHover : ApexUI::Palette::Surface;

	Frame->SetBrush(ApexUI::MakeBrush(Fill, Outline, bSelected ? 2.0f : 1.0f));
}

FReply UApexContentCardWidget::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (InMouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return FReply::Unhandled();
	}

	SetKeyboardFocus();
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
	ApplyState();
}

void UApexContentCardWidget::NativeOnRemovedFromFocusPath(const FFocusEvent& InFocusEvent)
{
	Super::NativeOnRemovedFromFocusPath(InFocusEvent);
	bFocused = false;
	ApplyState();
}

FReply UApexContentCardWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();
	if (Key == EKeys::Enter || Key == EKeys::SpaceBar || Key == EKeys::Virtual_Accept)
	{
		OnActivated.Broadcast(this);
		return FReply::Handled();
	}

	// Arrows belong to the grid, which is the screen.
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}
