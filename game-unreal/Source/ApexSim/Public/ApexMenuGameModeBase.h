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

	UFUNCTION(BlueprintPure, Category = "ApexSim|Menu")
	UApexRootWidget* GetRootWidget() const { return RootWidget; }

protected:
	/** Set to WBP_Root in BP_ApexMenuGameMode. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ApexSim|Menu")
	TSubclassOf<UApexRootWidget> RootWidgetClass;

private:
	UPROPERTY(Transient)
	TObjectPtr<UApexRootWidget> RootWidget;
};
