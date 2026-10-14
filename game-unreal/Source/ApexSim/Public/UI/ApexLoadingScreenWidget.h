#pragma once

#include "CoreMinimal.h"
#include "UI/ApexScreenWidget.h"

#include "ApexLoadingScreenWidget.generated.h"

class UTextBlock;

/**
 * Placeholder screen shown while something is in flight: one centred line
 * on the menu background. The shell has nothing slow enough to need it yet;
 * it keeps its EApexScreen slot so the switcher's indices stay put.
 */
UCLASS()
class APEXSIM_API UApexLoadingScreenWidget : public UApexScreenWidget
{
	GENERATED_BODY()

public:
	UApexLoadingScreenWidget(const FObjectInitializer& ObjectInitializer);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void SetMessage(const FString& Message);

protected:
	virtual void NativeOnInitialized() override;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> MessageText;
};
