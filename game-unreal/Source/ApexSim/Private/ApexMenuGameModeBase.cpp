#include "ApexMenuGameModeBase.h"

#include "ApexPlayerController.h"
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
	// Driving input lives on the controller, since there is no pawn to put it on.
	PlayerControllerClass = AApexPlayerController::StaticClass();
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

	// The constructor asks for AApexPlayerController, but a Blueprint subclass
	// can serialise its own PlayerControllerClass and quietly win. Without our
	// controller there is no Enhanced Input and no driving — and the failure
	// looks exactly like "the keys do nothing", which is expensive to chase.
	if (!PlayerController->IsA<AApexPlayerController>())
	{
		UE_LOG(LogApexSim, Error,
			TEXT("Player controller is %s, not AApexPlayerController — driving input will not "
				 "work. Clear the PlayerControllerClass override on BP_ApexMenuGameMode so it "
				 "inherits from the C++ game mode"),
			*PlayerController->GetClass()->GetName());
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

	// The starting state only. AApexPlayerController::SetDriveInputEnabled
	// switches to game-and-UI for the duration of a race and back again.
	FInputModeUIOnly InputMode;
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	PlayerController->SetInputMode(InputMode);
	PlayerController->bShowMouseCursor = true;

	UE_LOG(LogApexSim, Log, TEXT("Menu shell ready"));
}
