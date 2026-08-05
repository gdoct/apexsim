#include "UI/ApexToastWidget.h"

#include "Components/TextBlock.h"

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
