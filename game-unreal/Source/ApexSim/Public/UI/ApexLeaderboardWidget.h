#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "UI/ApexScreenWidget.h"

#include "ApexLeaderboardWidget.generated.h"

class UApexButtonWidget;
class UScrollBox;
class UTextBlock;

/**
 * A circuit's fastest laps as the server stores them (`RequestLeaderboard`):
 * each driver's best, every car or the picked one, best first with the gap to
 * the leader. Reached from Garage > Tracks. The circuit and the car filter
 * step with their buttons (Left and Right on a focused one); the player's own
 * laps are lit. Read again whenever the screen opens or a choice changes.
 */
UCLASS()
class APEXSIM_API UApexLeaderboardWidget : public UApexScreenWidget
{
	GENERATED_BODY()

public:
	UApexLeaderboardWidget(const FObjectInitializer& ObjectInitializer);

	virtual void OnScreenActivated() override;
	virtual void OnScreenDeactivated() override;
	virtual void FocusDefault() override;
	virtual bool WantsLiveBackdrop() const override { return false; }

	/** The lap rows as the list draws them, for the tests: rank, driver, car, time, gap, when. */
	struct FRow
	{
		int32 Rank = 0;
		FString Driver;
		FString Car;
		FString Time;
		FString Gap;
		FString When;
		bool bMine = false;
	};
	/** The board's lines, best first, with the gap to the fastest; pure. */
	static TArray<FRow> MakeRows(const TArray<FApexLeaderboardEntry>& Entries, const FString& Me,
		TFunctionRef<FString(const FString& CarId)> CarName);

protected:
	virtual void NativeOnInitialized() override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

private:
	void BuildLayout();
	/** Ask for the board of the chosen circuit and car; the list shows what the server last said. */
	void Request();
	void Rebuild();
	void StepTrack(int32 Direction);
	void ToggleCar();
	void UpdateButtons();
	/** Take the circuits from the lobby's list, starting on the garage's pick. */
	void LoadTracks();

	UFUNCTION() void HandleButtonActivated(UApexButtonWidget* Button);
	UFUNCTION() void HandleLeaderboard(const FString& TrackId, const FString& CarId);
	/** The lobby's list arrives after a screen opened at startup: the circuits are read again then. */
	UFUNCTION() void HandleLobbyState(const FApexLobbyState& LobbyState);

	UPROPERTY(Transient) TObjectPtr<UScrollBox> List;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> StatusText;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> TrackButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> CarButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> BackButton;

	/** The circuits this machine can name (server ids, by name) and the one shown. */
	TArray<FString> TrackIds;
	int32 TrackIndex = 0;
	/** Only the car being set up in the garage, instead of every car. */
	bool bOneCar = false;
	bool bBound = false;
};
