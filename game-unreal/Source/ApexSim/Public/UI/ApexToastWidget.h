#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"

#include "ApexToastWidget.generated.h"

class UTextBlock;

/** Transient message strip. Errors from the server surface here. */
UCLASS()
class APEXSIM_API UApexToastWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void Show(const FString& Message, bool bIsError);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void Hide();

protected:
	UPROPERTY(Transient, BlueprintReadOnly, Category = "ApexSim|UI")
	TObjectPtr<UTextBlock> MessageText;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|UI")
	float DisplaySeconds = 4.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|UI")
	FLinearColor NormalColor = FLinearColor(0.90f, 0.90f, 0.90f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|UI")
	FLinearColor ErrorColor = FLinearColor(0.95f, 0.45f, 0.40f);

private:
	float RemainingSeconds = 0.0f;
};
