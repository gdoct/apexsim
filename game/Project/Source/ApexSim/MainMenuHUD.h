// Copyright ApexSim Team. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "MainMenuHUD.generated.h"

/**
 * HUD for main menu scene
 */
UCLASS()
class APEXSIM_API AMainMenuHUD : public AHUD
{
	GENERATED_BODY()

public:
	AMainMenuHUD();

protected:
	virtual void BeginPlay() override;
};
