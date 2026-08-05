#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "Blueprint/UserWidget.h"
#include "Catalog/ApexCatalogRows.h"

#include "ApexCarCardWidget.generated.h"

class UApexCarCardWidget;
class UBorder;
class UButton;
class UTextBlock;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FApexOnCarCardClicked, UApexCarCardWidget*, Card);

/**
 * One row in the car list.
 *
 * Text only — the 3D preview lives on the shared AApexCarPreviewStage and is
 * shown once, for the selected car, on the right of the screen.
 */
UCLASS(Abstract)
class APEXSIM_API UApexCarCardWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	virtual void NativeConstruct() override;

	/** Fills the card. CatalogRow supplies brand/class/year; Summary is the server's view. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void SetCar(const FApexCarConfigSummary& Summary, const FApexCarCatalogRow& CatalogRow, bool bHasCatalogRow);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void SetSelected(bool bInSelected);

	UFUNCTION(BlueprintPure, Category = "ApexSim|UI")
	const FString& GetCarId() const { return CarId; }

	UFUNCTION(BlueprintPure, Category = "ApexSim|UI")
	const FString& GetDisplayName() const { return DisplayName; }

	UPROPERTY(BlueprintAssignable, Category = "ApexSim|UI")
	FApexOnCarCardClicked OnCardClicked;

protected:
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UButton> SelectButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UBorder> HighlightBorder;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UTextBlock> CarNameText;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UTextBlock> CarSpecsText;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|UI")
	FLinearColor SelectedTint = FLinearColor(0.15f, 0.45f, 0.75f, 0.85f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|UI")
	FLinearColor UnselectedTint = FLinearColor(0.08f, 0.08f, 0.10f, 0.65f);

private:
	UFUNCTION()
	void HandleClicked();

	FString CarId;
	FString DisplayName;
	bool bSelected = false;
};
