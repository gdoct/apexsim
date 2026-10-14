#include "UI/ApexLoadingScreenWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "UI/ApexUIStyle.h"

UApexLoadingScreenWidget::UApexLoadingScreenWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Nothing on it takes focus, so the screen itself does: a pad's B (and
	// Escape) only reach a surface that holds the keys.
	SetIsFocusable(true);
}

void UApexLoadingScreenWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	MessageText = ApexUI::MakeText(*WidgetTree, TEXT("LOADING"), ApexUI::Font::Display(40.0f, 120), ApexUI::Palette::TextSecondary);
	UVerticalBox* Centre = WidgetTree->ConstructWidget<UVerticalBox>();
	ApexUI::AddV(Centre, WidgetTree->ConstructWidget<USpacer>(), FMargin(), HAlign_Fill, 1.0f);
	ApexUI::AddV(Centre, MessageText, FMargin(), HAlign_Center);
	ApexUI::AddV(Centre, WidgetTree->ConstructWidget<USpacer>(), FMargin(), HAlign_Fill, 1.0f);
	WidgetTree->RootWidget = ApexUI::MakePanel(*WidgetTree, Centre, FMargin(), ApexUI::MakeBrush(ApexUI::Palette::Background));
}

void UApexLoadingScreenWidget::SetMessage(const FString& Message)
{
	if (MessageText)
	{
		MessageText->SetText(FText::FromString(Message));
	}
}
