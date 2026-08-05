#include "UI/ApexScreenWidget.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "Blueprint/WidgetTree.h"
#include "Engine/GameInstance.h"
#include "UI/ApexRootWidget.h"

void UApexScreenWidget::OnScreenActivated()
{
	BP_OnScreenActivated();
}

void UApexScreenWidget::OnScreenDeactivated()
{
	BP_OnScreenDeactivated();
}

UApexNetSubsystem* UApexScreenWidget::GetNet() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UApexNetSubsystem>() : nullptr;
}

UApexMenuFlowSubsystem* UApexScreenWidget::GetFlow() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UApexMenuFlowSubsystem>() : nullptr;
}

UApexRootWidget* UApexScreenWidget::GetRoot() const
{
	// Screens are nested inside the root's switcher, so walk up rather than
	// caching a pointer that would go stale if the tree is rebuilt.
	for (UWidget* Parent = GetParent(); Parent; Parent = Parent->GetParent())
	{
		if (UApexRootWidget* Root = Cast<UApexRootWidget>(Parent))
		{
			return Root;
		}
	}

	// A screen added directly to the viewport (or during authoring) has no
	// root above it; fall back to the one the game mode created.
	if (const UWorld* World = GetWorld())
	{
		for (TObjectIterator<UApexRootWidget> It; It; ++It)
		{
			if (It->GetWorld() == World && It->IsInViewport())
			{
				return *It;
			}
		}
	}
	return nullptr;
}

void UApexScreenWidget::ShowScreen(EApexScreen Screen)
{
	if (UApexRootWidget* Root = GetRoot())
	{
		Root->ShowScreen(Screen);
	}
}

void UApexScreenWidget::GoBack()
{
	if (UApexRootWidget* Root = GetRoot())
	{
		Root->GoBack();
	}
}

void UApexScreenWidget::ShowToast(const FString& Message, bool bIsError)
{
	if (UApexRootWidget* Root = GetRoot())
	{
		Root->ShowToast(Message, bIsError);
	}
}
