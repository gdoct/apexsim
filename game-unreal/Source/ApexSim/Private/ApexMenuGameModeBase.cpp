#include "ApexMenuGameModeBase.h"

#include "ApexSim.h"
#include "Blueprint/UserWidget.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "UI/ApexRootWidget.h"

AApexMenuGameModeBase::AApexMenuGameModeBase()
{
	// A menu needs neither. Leaving DefaultPawnClass at its default would spawn
	// a DefaultPawn that captures input away from the UI.
	DefaultPawnClass = nullptr;
	HUDClass = nullptr;
	// PlayerStateClass is deliberately left at its default: nulling it makes
	// AGameSession log "Player State class is invalid for game mode".
}

void AApexMenuGameModeBase::BeginPlay()
{
	Super::BeginPlay();

	APlayerController* PlayerController = UGameplayStatics::GetPlayerController(this, 0);
	if (!PlayerController)
	{
		UE_LOG(LogApexSim, Error, TEXT("No player controller; the menu cannot be shown"));
		return;
	}

	if (!RootWidgetClass)
	{
		UE_LOG(LogApexSim, Error,
			TEXT("RootWidgetClass is unset on %s — set it to WBP_Root in BP_ApexMenuGameMode"), *GetName());
		return;
	}

	RootWidget = CreateWidget<UApexRootWidget>(PlayerController, RootWidgetClass);
	if (!RootWidget)
	{
		UE_LOG(LogApexSim, Error, TEXT("Failed to create the root menu widget"));
		return;
	}

	RootWidget->AddToViewport(0);

	FInputModeUIOnly InputMode;
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	PlayerController->SetInputMode(InputMode);
	PlayerController->bShowMouseCursor = true;

	UE_LOG(LogApexSim, Log, TEXT("Menu shell ready"));
}
