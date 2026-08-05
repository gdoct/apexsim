#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "UI/ApexScreenWidget.h"

#include "ApexTrackSelectWidget.generated.h"

class UApexTrackCardWidget;
class UButton;
class UEditableTextBox;
class UWrapBox;
class UTextBlock;

/**
 * Track picker. Entirely client-side: the protocol has no SelectTrack message,
 * so the choice is stashed on the flow subsystem and only reaches the server
 * as an argument to CreateSession.
 */
UCLASS(Abstract)
class APEXSIM_API UApexTrackSelectWidget : public UApexScreenWidget
{
	GENERATED_BODY()

public:
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual void OnScreenActivated() override;

protected:
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UWrapBox> TrackGrid;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UEditableTextBox> SearchBox;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UTextBlock> StatusText;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UButton> ConfirmButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UButton> BackButton;

	/** Set to WBP_TrackCard in the WBP defaults. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ApexSim|UI")
	TSubclassOf<UApexTrackCardWidget> TrackCardClass;

private:
	UFUNCTION() void HandleCardClicked(UApexTrackCardWidget* Card);
	UFUNCTION() void HandleConfirmClicked();
	UFUNCTION() void HandleBackClicked();
	UFUNCTION() void HandleSearchChanged(const FText& Text);
	UFUNCTION() void HandleLobbyStateUpdated(const FApexLobbyState& LobbyState);

	void RebuildCards();
	void ApplyFilter();
	void SelectTrack(const FString& TrackId);

	UPROPERTY(Transient)
	TArray<TObjectPtr<UApexTrackCardWidget>> Cards;

	FString SelectedTrackId;
	/** 26 cards is enough that rebuilding them twice a second would be visible. */
	TArray<FString> BuiltTrackIds;
};
