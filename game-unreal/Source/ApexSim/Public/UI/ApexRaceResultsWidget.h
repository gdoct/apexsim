#pragma once

#include "CoreMinimal.h"
#include "UI/ApexNavigation.h"

#include "ApexRaceResultsWidget.generated.h"

struct FApexCarResult;
class UApexButtonWidget;
class UBorder;
class UOverlay;
class UTextBlock;
class UVerticalBox;
class UWidget;

/** What the results over the race ask the shell to do. */
UENUM(BlueprintType)
enum class EApexRaceResultsAction : uint8
{
	/** Line the session up again (only once the race has ended). */
	DriveAgain,
	/** Leave the race view for the session's lobby. */
	BackToLobby,
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FApexOnRaceResultsAction, EApexRaceResultsAction, Action);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FApexOnRaceResultsWatch, int32, CarIndex);

/** The table's rows, pure, so the rules are tested without a widget. */
namespace ApexRaceResults
{
	struct FRow
	{
		int32 CarIndex = INDEX_NONE;
		/** 1-based: the classification for a finisher, the running order otherwise. */
		int32 Position = 0;
		FString Name;
		bool bAi = false;
		bool bYou = false;
		bool bFinished = false;
		int32 Laps = 0;
		float BestLapSeconds = 0.0f;
		/** The winner's race time, a finisher's gap to it, or the state of a car without a flag. */
		FString Time;
		/** A click films this car: still racing (or our own, back to the panorama). */
		bool bWatchable = false;
	};

	/**
	 * Rows in the recorder's order (finishers by classification, the rest in
	 * the running order behind them). `bLive` is whether the race is still
	 * running: a car without a flag is then on track and can be watched;
	 * after the end it did not finish.
	 */
	APEXSIM_API TArray<FRow> BuildRows(TConstArrayView<FApexCarResult> Results, const FString& LocalPlayerId, bool bLive);

	/** 1:23.456 or 1:02:03.456. */
	APEXSIM_API FString FormatRaceTime(float Seconds);
}

/**
 * The results over the race, once the local car has taken the flag.
 *
 * The race view stays on behind it, filmed by the race director's finish view
 * (the panorama over the player's car, driven on by the server's cool-down
 * driver), and the card is translucent so the race shows through. Until the
 * session ends the table is provisional and follows the field home; a driver
 * still racing can be clicked to film their car (OnWatchCar), the player's own
 * row brings the panorama back.
 *
 * Like the pause menu it reports what was chosen and lets the root widget
 * carry it out. H (pad Y) folds it down to a tab, to watch the race.
 */
UCLASS()
class APEXSIM_API UApexRaceResultsWidget : public UApexNavigableWidget
{
	GENERATED_BODY()

public:
	UApexRaceResultsWidget(const FObjectInitializer& ObjectInitializer);

	virtual void NativeOnInitialized() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

	/** Drive again once the race is over; the first driver still racing before. */
	virtual void FocusDefault() override;
	virtual bool HandleNavigation(EUINavigation Direction, UWidget* Source) override;
	/** B folds the card away rather than leaving: there is nothing behind it to go back to. */
	virtual bool HandleBack() override;

	UPROPERTY(BlueprintAssignable, Category = "ApexSim|UI")
	FApexOnRaceResultsAction OnAction;

	/** A row was clicked: the car to film, INDEX_NONE (or the local car) for the panorama. */
	UPROPERTY(BlueprintAssignable, Category = "ApexSim|UI")
	FApexOnRaceResultsWatch OnWatchCar;

	void Open();
	void Close();
	bool IsOpen() const { return bOpen; }

	/** Folded down to a tab in the corner, or the whole card. */
	void SetMinimised(bool bInMinimised);
	bool IsMinimised() const { return bMinimised; }

	/** The car the camera is on, for the row's highlight. */
	void SetWatchedCar(int32 CarIndex);

private:
	UFUNCTION()
	void HandleButtonActivated(UApexButtonWidget* Button);

	void Refresh();
	/** One more row in the pool. */
	void AddRow();
	void RefreshSaveReplay();
	bool IsLive() const;

	UPROPERTY(Transient) TObjectPtr<UWidget> Card;
	UPROPERTY(Transient) TObjectPtr<UWidget> Tab;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> TabButton;

	UPROPERTY(Transient) TObjectPtr<UTextBlock> StatusText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> TitleText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> FormatText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> HintText;

	UPROPERTY(Transient) TObjectPtr<UVerticalBox> RowBox;
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> RowButtons;
	UPROPERTY(Transient) TArray<TObjectPtr<UWidget>> RowRoots;
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> RowPos;
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> RowName;
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> RowLaps;
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> RowTime;
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> RowBest;
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> RowState;
	/** The car each pooled row shows, INDEX_NONE when unused. */
	TArray<int32> RowCars;
	TArray<bool> RowWatchable;

	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> DriveAgainButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> LobbyButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> SaveReplayButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> HideButton;

	bool bOpen = false;
	bool bMinimised = false;
	int32 WatchedCar = INDEX_NONE;
	float RefreshCountdown = 0.0f;
	/** What the last refresh showed, so the end of the race moves focus once. */
	bool bShowedLive = true;
};
