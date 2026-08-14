#include "UI/ApexToastWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/TextBlock.h"
#include "UI/ApexUIStyle.h"

void UApexToastWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	MessageText = ApexUI::MakeText(*WidgetTree, FString(), ApexUI::Font::Body(14.0f), NormalColor);

	WidgetTree->RootWidget = ApexUI::MakePanel(
		*WidgetTree,
		MessageText,
		FMargin(22.0f, 12.0f),
		ApexUI::MakeBrush(ApexUI::Palette::Surface, ApexUI::Palette::Border, 1.0f));
}

void UApexToastWidget::NativeConstruct()
{
	Super::NativeConstruct();
	Hide();
}

void UApexToastWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	if (RemainingSeconds > 0.0f)
	{
		RemainingSeconds -= InDeltaTime;
		if (RemainingSeconds <= 0.0f)
		{
			Hide();
		}
	}
}

void UApexToastWidget::Show(const FString& Message, bool bIsError)
{
	if (MessageText)
	{
		MessageText->SetText(FText::FromString(Message));
		MessageText->SetColorAndOpacity(bIsError ? ErrorColor : NormalColor);
	}
	RemainingSeconds = DisplaySeconds;
	// HitTestInvisible, not Visible: a toast must never eat a click aimed at
	// the button underneath it.
	SetVisibility(ESlateVisibility::HitTestInvisible);
}

void UApexToastWidget::Hide()
{
	RemainingSeconds = 0.0f;
	SetVisibility(ESlateVisibility::Collapsed);
}
