#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "UI/ApexScreenWidget.h"

#include "ApexSessionCreateWidget.generated.h"

class UButton;
class UComboBoxString;
class USpinBox;
class UTextBlock;

/** Track + car pickers and the CreateSession parameters. */
UCLASS(Abstract)
class APEXSIM_API UApexSessionCreateWidget : public UApexScreenWidget
{
	GENERATED_BODY()

public:
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual void OnScreenActivated() override;

protected:
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UButton> TrackSelectorButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UTextBlock> TrackSelectorLabel;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UButton> CarSelectorButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UTextBlock> CarSelectorLabel;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UComboBoxString> SessionKindCombo;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<USpinBox> MaxPlayersSpin;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<USpinBox> AiCountSpin;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<USpinBox> LapLimitSpin;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UButton> CreateButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UButton> BackButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UTextBlock> StatusText;

private:
	UFUNCTION() void HandleTrackSelectorClicked();
	UFUNCTION() void HandleCarSelectorClicked();
	UFUNCTION() void HandleCreateClicked();
	UFUNCTION() void HandleBackClicked();
	UFUNCTION() void HandleKindChanged(FString SelectedItem, ESelectInfo::Type SelectionType);

	void RefreshSelections();
	void CommitSpinBoxes();
	static EApexSessionKind KindFromLabel(const FString& Label);
	static FString LabelFromKind(EApexSessionKind Kind);
};
