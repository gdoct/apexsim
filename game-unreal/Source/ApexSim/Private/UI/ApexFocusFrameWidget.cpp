#include "UI/ApexFocusFrameWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "UI/ApexUIStyle.h"

void UApexFocusFrameWidget::SetFramedContent(UWidget* Content, const FMargin& RingPadding)
{
	PendingContent = Content;
	PendingPadding = RingPadding;
	Build();
}

void UApexFocusFrameWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	Build();
}

void UApexFocusFrameWidget::Build()
{
	// Content is handed over before or after initialisation depending on how the
	// frame was made; either way the tree is built once, when both exist.
	if (!WidgetTree || !PendingContent || Host)
	{
		return;
	}

	Host = WidgetTree->ConstructWidget<UOverlay>();
	UOverlaySlot* ContentSlot = Host->AddChildToOverlay(PendingContent);
	ContentSlot->SetHorizontalAlignment(HAlign_Fill);
	ContentSlot->SetVerticalAlignment(VAlign_Fill);
	Ring = ApexUI::AddFocusRing(*WidgetTree, *Host, PendingPadding);
	WidgetTree->RootWidget = Host;
}

void UApexFocusFrameWidget::NativeOnAddedToFocusPath(const FFocusEvent& InFocusEvent)
{
	Super::NativeOnAddedToFocusPath(InFocusEvent);
	if (Ring)
	{
		Ring->SetVisibility(ESlateVisibility::HitTestInvisible);
	}
}

void UApexFocusFrameWidget::NativeOnRemovedFromFocusPath(const FFocusEvent& InFocusEvent)
{
	Super::NativeOnRemovedFromFocusPath(InFocusEvent);
	if (Ring)
	{
		Ring->SetVisibility(ESlateVisibility::Hidden);
	}
}
