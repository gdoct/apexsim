// Copyright ApexSim Team. All Rights Reserved.

#include "ApexSimGameInstance.h"
#include "Blueprint/UserWidget.h"
#include "TimerManager.h"
#include "Kismet/GameplayStatics.h"

UApexSimGameInstance::UApexSimGameInstance()
{
	LoadingScreenDuration = 2.0f;
}

void UApexSimGameInstance::Init()
{
	Super::Init();

	// Show loading screen on startup
	if (LoadingScreenWidgetClass)
	{
		CurrentWidget = CreateWidget<UUserWidget>(this, LoadingScreenWidgetClass);
		if (CurrentWidget)
		{
			CurrentWidget->AddToViewport(0);

			// Set input mode to UI only during loading
			if (APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0))
			{
				FInputModeUIOnly InputMode;
				InputMode.SetWidgetToFocus(CurrentWidget->TakeWidget());
				PC->SetInputMode(InputMode);
				PC->bShowMouseCursor = false;
			}

			// Set timer to transition to main menu
			GetTimerManager().SetTimer(
				LoadingTimerHandle,
				this,
				&UApexSimGameInstance::ShowMainMenu,
				LoadingScreenDuration,
				false
			);
		}
	}
}

void UApexSimGameInstance::ShowMainMenu()
{
	// Remove loading screen
	if (CurrentWidget)
	{
		CurrentWidget->RemoveFromParent();
		CurrentWidget = nullptr;
	}

	// Create and show main menu
	if (MainMenuWidgetClass)
	{
		CurrentWidget = CreateWidget<UUserWidget>(this, MainMenuWidgetClass);
		if (CurrentWidget)
		{
			CurrentWidget->AddToViewport(0);

			// Set input mode to UI only
			if (APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0))
			{
				FInputModeUIOnly InputMode;
				InputMode.SetWidgetToFocus(CurrentWidget->TakeWidget());
				PC->SetInputMode(InputMode);
				PC->bShowMouseCursor = true;
			}
		}
	}
}
