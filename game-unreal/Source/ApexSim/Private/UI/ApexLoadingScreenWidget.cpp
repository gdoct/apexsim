#include "UI/ApexLoadingScreenWidget.h"

#include "Components/TextBlock.h"

UApexLoadingScreenWidget::UApexLoadingScreenWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Nothing on it takes focus, so the screen itself does: a pad's B (and
	// Escape) only reach a surface that holds the keys.
	SetIsFocusable(true);
}

void UApexLoadingScreenWidget::SetMessage(const FString& Message)
{
	if (MessageText)
	{
		MessageText->SetText(FText::FromString(Message));
	}
}
