#pragma once

#include "CoreMinimal.h"
#include "UI/ApexScreenWidget.h"

#include "ApexLoadingScreenWidget.generated.h"

class UTextBlock;

/**
 * Placeholder screen shown while something is in flight. The shell has nothing
 * slow enough to need it yet; it exists so the switcher index layout is stable
 * once loading a track actually takes time.
 */
UCLASS(Abstract)
class APEXSIM_API UApexLoadingScreenWidget : public UApexScreenWidget
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void SetMessage(const FString& Message);

protected:
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UTextBlock> MessageText;
};
