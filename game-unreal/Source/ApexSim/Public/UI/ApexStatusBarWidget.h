#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "Blueprint/UserWidget.h"

#include "ApexStatusBarWidget.generated.h"

class UTextBlock;

/** Persistent connection indicator in the corner of every screen. */
UCLASS(Abstract)
class APEXSIM_API UApexStatusBarWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;

protected:
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UTextBlock> StatusText;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|UI")
	FLinearColor ConnectedColor = FLinearColor(0.25f, 0.85f, 0.35f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|UI")
	FLinearColor PendingColor = FLinearColor(0.95f, 0.80f, 0.25f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|UI")
	FLinearColor FailedColor = FLinearColor(0.90f, 0.30f, 0.30f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|UI")
	FLinearColor IdleColor = FLinearColor(0.60f, 0.60f, 0.60f);

private:
	UFUNCTION()
	void HandleConnectionStateChanged(EApexConnectionState NewState, const FString& Detail);

	void Refresh();
};
