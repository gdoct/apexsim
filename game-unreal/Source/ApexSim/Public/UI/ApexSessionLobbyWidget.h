#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "UI/ApexScreenWidget.h"

#include "ApexSessionLobbyWidget.generated.h"

class UApexPlayerRowWidget;
class UApexTrackCardWidget;
class UButton;
class UVerticalBox;
class UTextBlock;

/**
 * The pre-race lobby: who is in, on what track, in what car.
 *
 * The Start Race button here is deliberately inert — see HandleStartRaceClicked.
 */
UCLASS(Abstract)
class APEXSIM_API UApexSessionLobbyWidget : public UApexScreenWidget
{
	GENERATED_BODY()

public:
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual void OnScreenActivated() override;

protected:
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UTextBlock> SessionInfoText;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UTextBlock> GameModeText;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UTextBlock> StatusText;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UTextBlock> SelectedCarText;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UVerticalBox> PlayersContainer;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UApexTrackCardWidget> TrackCard;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UButton> ChangeCarButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UButton> StartRaceButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "ApexSim|UI")
	TObjectPtr<UButton> LeaveButton;

	/** Set to WBP_PlayerRow in the WBP defaults. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ApexSim|UI")
	TSubclassOf<UApexPlayerRowWidget> PlayerRowClass;

	/** Grid countdown before the race begins, in seconds. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|UI")
	int32 StartCountdownSeconds = 5;

private:
	UFUNCTION() void HandleChangeCarClicked();
	UFUNCTION() void HandleStartRaceClicked();
	UFUNCTION() void HandleLeaveClicked();
	UFUNCTION() void HandleLobbyStateUpdated(const FApexLobbyState& LobbyState);
	UFUNCTION() void HandleGameModeChanged(EApexGameMode NewMode);
	UFUNCTION() void HandleCountdownUpdate(int32 SecondsRemaining);

	void Refresh();
	static FString DescribeGameMode(EApexGameMode Mode);

	UPROPERTY(Transient)
	TArray<TObjectPtr<UApexPlayerRowWidget>> PlayerRows;

	int32 BuiltPlayerRowCount = 0;
};
