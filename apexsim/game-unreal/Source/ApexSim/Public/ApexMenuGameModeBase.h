#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"

#include "ApexMenuGameModeBase.generated.h"

class UApexRootWidget;

/**
 * Boots the menu: creates the root widget, puts the viewport into UI-only
 * input, and shows the cursor. No pawn, no HUD — nothing in the shell is
 * driven by gameplay input.
 */
UCLASS()
class APEXSIM_API AApexMenuGameModeBase : public AGameModeBase
{
	GENERATED_BODY()

public:
	AApexMenuGameModeBase();

	virtual void BeginPlay() override;

	/**
	 * A side viewer of a triple-monitor rig gets AApexSideViewController
	 * instead of the driving controller, while UApexMultiViewSubsystem says
	 * one is being added; everybody else gets PlayerControllerClass.
	 */
	virtual APlayerController* SpawnPlayerController(ENetRole InRemoteRole, const FString& Options) override;

	UFUNCTION(BlueprintPure, Category = "ApexSim|Menu")
	UApexRootWidget* GetRootWidget() const { return RootWidget; }

protected:
	/** Optional. Unset means the C++ shell (UApexRootWidget) is used directly. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ApexSim|Menu")
	TSubclassOf<UApexRootWidget> RootWidgetClass;

private:
	UPROPERTY(Transient)
	TObjectPtr<UApexRootWidget> RootWidget;
};
