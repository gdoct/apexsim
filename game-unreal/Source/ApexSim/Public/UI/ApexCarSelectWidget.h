#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "UI/ApexScreenWidget.h"

#include "ApexCarSelectWidget.generated.h"

class UApexCarCardWidget;
class UButton;
class UEditableTextBox;
class UImage;
class UVerticalBox;
class UTextBlock;

/**
 * Car picker: filterable list on the left, turntable render plus specs on the
 * right. Confirming sends SelectCar and returns to whichever screen asked.
 */
UCLASS(Abstract)
class APEXSIM_API UApexCarSelectWidget : public UApexScreenWidget
{
	GENERATED_BODY()

public:
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual void OnScreenActivated() override;
	virtual void OnScreenDeactivated() override;

protected:
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UVerticalBox> CarList;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UEditableTextBox> SearchBox;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UImage> CarPreviewImage;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UTextBlock> SpecsText;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UTextBlock> StatusText;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UButton> ConfirmButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UButton> BackButton;

	/** Set to WBP_CarCard in the WBP defaults. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ApexSim|UI")
	TSubclassOf<UApexCarCardWidget> CarCardClass;

	/** Edge length of the square turntable preview, in slate units. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|UI")
	float PreviewSize = 420.0f;

private:
	UFUNCTION() void HandleCardClicked(UApexCarCardWidget* Card);
	UFUNCTION() void HandleConfirmClicked();
	UFUNCTION() void HandleBackClicked();
	UFUNCTION() void HandleSearchChanged(const FText& Text);
	UFUNCTION() void HandleLobbyStateUpdated(const FApexLobbyState& LobbyState);

	/**
	 * Rebuilds the card list. Called once per catalog change, not on every
	 * LobbyState — the server broadcasts one every ~2s and tearing down 4 cards
	 * that often would fight the user's hover and scroll position.
	 */
	void RebuildCards();
	void ApplyFilter();
	void SelectCar(const FString& CarId);
	void UpdatePreview();

	UPROPERTY(Transient)
	TArray<TObjectPtr<UApexCarCardWidget>> Cards;

	FString SelectedCarId;
	/** Guards against rebuilding when the car list has not actually changed. */
	TArray<FString> BuiltCarIds;
};
