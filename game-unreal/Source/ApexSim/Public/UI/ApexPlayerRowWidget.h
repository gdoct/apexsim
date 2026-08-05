#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "Blueprint/UserWidget.h"

#include "ApexPlayerRowWidget.generated.h"

class UTextBlock;

/** One participant in the session lobby, with a car-ready tick. */
UCLASS(Abstract)
class APEXSIM_API UApexPlayerRowWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void SetPlayer(const FApexLobbyPlayer& Player, bool bIsLocalPlayer);

protected:
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UTextBlock> PlayerNameText;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UTextBlock> ReadyGlyphText;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|UI")
	FLinearColor ReadyColor = FLinearColor(0.30f, 0.85f, 0.40f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|UI")
	FLinearColor NotReadyColor = FLinearColor(0.55f, 0.55f, 0.55f);
};
