#include "UI/ApexLoadingScreenWidget.h"

#include "Components/TextBlock.h"

void UApexLoadingScreenWidget::SetMessage(const FString& Message)
{
	if (MessageText)
	{
		MessageText->SetText(FText::FromString(Message));
	}
}
