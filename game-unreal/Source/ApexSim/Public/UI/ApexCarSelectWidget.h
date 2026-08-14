#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "Catalog/ApexCatalogRows.h"
#include "UI/ApexScreenWidget.h"

#include "ApexCarSelectWidget.generated.h"

class UApexButtonWidget;
class UHorizontalBox;
class UImage;
class UProgressBar;
class UScrollBox;
class UTextBlock;
class UVerticalBox;
class UWidget;

/**
 * The garage: every car the server offers, one described at a time.
 *
 * The preview is the shared AApexCarPreviewStage turntable rendered into a
 * render target — one scene capture for the screen rather than one per car.
 *
 * The stat bars are relative, not absolute: a car's power bar is its share of
 * the most powerful car available. Absolute bars would need a scale nothing in
 * the data defines.
 */
UCLASS()
class APEXSIM_API UApexCarSelectWidget : public UApexScreenWidget
{
	GENERATED_BODY()

public:
	UApexCarSelectWidget(const FObjectInitializer& ObjectInitializer);

	virtual void OnScreenActivated() override;
	virtual void OnScreenDeactivated() override;

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

private:
	void BuildLayout();
	UWidget* BuildHeader();
	UWidget* BuildStage();

	void RebuildList(bool bForce = false);
	void RebuildFilterChips();
	void ApplyFilter();
	void SelectCar(const FString& CarId);
	void RefreshDetail();

	/** Points the shared turntable at the selected car's mesh. */
	void UpdatePreviewStage();

	/** Focus has to wait a tick after the tree changes; see the main menu. */
	void RequestRowFocus();

	UFUNCTION() void HandleLobbyStateUpdated(const FApexLobbyState& LobbyState);
	UFUNCTION() void HandleRowActivated(UApexButtonWidget* Row);
	UFUNCTION() void HandleButtonActivated(UApexButtonWidget* Button);

	void MoveSelection(int32 Delta);

	/** Horsepower from the catalog's kW, or 0 when there is no row. */
	static float GetPowerHp(const FApexCarCatalogRow& Row);

	UPROPERTY(Transient) TObjectPtr<UVerticalBox> CarListBox;
	UPROPERTY(Transient) TObjectPtr<UScrollBox> CarListScroll;
	UPROPERTY(Transient) TObjectPtr<UHorizontalBox> ChipRow;
	UPROPERTY(Transient) TObjectPtr<UImage> StageImage;

	UPROPERTY(Transient) TObjectPtr<UTextBlock> EyebrowText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> NameText;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> DriveButton;

	UPROPERTY(Transient) TObjectPtr<UTextBlock> PowerValue;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> WeightValue;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> RatioValue;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> ClassValue;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> PowerBar;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> WeightBar;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> RatioBar;

	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> CarRows;
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> VisibleRows;
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> FilterChips;

	TArray<FString> BuiltCarIds;
	FString SelectedCarId;
	FString ActiveFilter;
	int32 FocusedRowIndex = 0;
};
