// Copyright ApexSim Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "ApexSimGameInstance.generated.h"

/**
 * Game Instance managing startup flow and global state
 */
UCLASS()
class APEXSIM_API UApexSimGameInstance : public UGameInstance
{
	GENERATED_BODY()

public:
	UApexSimGameInstance();

	virtual void Init() override;

	/** Transitions from loading screen to main menu */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void ShowMainMenu();

protected:
	/** Widget class for the loading screen */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "UI")
	TSubclassOf<class UUserWidget> LoadingScreenWidgetClass;

	/** Widget class for the main menu */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "UI")
	TSubclassOf<class UUserWidget> MainMenuWidgetClass;

	/** Duration to show loading screen (seconds) */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "UI")
	float LoadingScreenDuration = 2.0f;

private:
	UPROPERTY()
	TObjectPtr<UUserWidget> CurrentWidget;

	FTimerHandle LoadingTimerHandle;
};
