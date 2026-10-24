#pragma once

#include "CoreMinimal.h"
#include "Season/ApexSeason.h"
#include "UI/ApexScreenWidget.h"

#include "ApexSeasonWidget.generated.h"

class UApexButtonWidget;
class UApexStepperWidget;
class UScrollBox;
class UTextBlock;
class UVerticalBox;

/**
 * An offline season against the AI (Season/ApexSeason.h, saved in the
 * driver's profile). Without one: a setup card (class, car, rounds, points,
 * laps, field, AI level, dropped rounds, ballast, the fastest lap's points)
 * and the calendar it makes, each round a row that opens its own rules
 * (weather, start, qualifying, length). With
 * one: the calendar, the drivers' and cars' standings, and Race round N,
 * which creates the round's race and counts it in; the session recorder
 * files the classification when it takes the flag. Main menu > Drive >
 * Season.
 */
UCLASS()
class APEXSIM_API UApexSeasonWidget : public UApexScreenWidget
{
	GENERATED_BODY()

public:
	UApexSeasonWidget(const FObjectInitializer& ObjectInitializer);

	virtual void OnScreenActivated() override;
	virtual void FocusDefault() override;
	virtual bool HandleNavigation(EUINavigation Direction, UWidget* Source) override;
	/** Back closes a round's rules before it leaves the screen. */
	virtual bool HandleBack() override;
	virtual bool WantsLiveBackdrop() const override { return false; }

	/** The setup's laps, field and rounds, as `apexsim.season.Setup` sets them. */
	void SetSetup(int32 InLaps, int32 InAi, int32 InRounds, const FString& FirstStem = FString(), int32 InBallast = 0);
	/** The main button, as `apexsim.season.Race` presses it. */
	void PressPrimary();
	/** The fastest lap's points (0-5) and whether they need a points finish, as `apexsim.season.FastestLap` sets them. */
	void SetFastestLap(int32 InPoints, bool bInNeedsPoints);
	/**
	 * Round `Round`'s (0-based) own rules in the setup, as
	 * `apexsim.season.Round` sets them (FApexSeasonRound: -1 / 0 for the
	 * season's), and that round's rules opened.
	 */
	void SetRoundRules(int32 Round, int32 InWeather, int32 InStartHour, int32 InQualifyingMinutes, int32 InLaps, int32 InRaceMinutes);
	/** Open round `Round`'s (0-based) rules in the setup; INDEX_NONE back to the calendar. */
	void EditRound(int32 Round);

protected:
	virtual void NativeOnInitialized() override;

private:
	void BuildLayout();
	void Rebuild();
	void RebuildSetup();
	void RebuildActive();
	/** The setup's right column: the calendar, or the round being edited's rules. */
	void RebuildCalendar(const FApexSeason& Preview);
	void RebuildRoundEditor(const FApexSeason& Preview);
	/** The round's rules in the setup, grown to hold it. */
	FApexSeasonRound& RulesOf(int32 Round);
	/** The circuits and cars this server and machine can both name, by display name. */
	void LoadContent();
	/** The season the setup steppers describe. */
	FApexSeason SeasonFromSetup() const;
	void RaceNextRound();

	UFUNCTION() void HandleButtonActivated(UApexButtonWidget* Button);
	UFUNCTION() void HandleStepperChanged(UApexStepperWidget* Control, int32 Value);
	UFUNCTION() void HandleLobbyState(const FApexLobbyState& LobbyState);

	UPROPERTY(Transient) TObjectPtr<UTextBlock> HeadingText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> StatusText;
	UPROPERTY(Transient) TObjectPtr<UVerticalBox> LeftColumn;
	UPROPERTY(Transient) TObjectPtr<UVerticalBox> RightColumn;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> PrimaryButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> EndButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> BackButton;
	UPROPERTY(Transient) TArray<TObjectPtr<UApexStepperWidget>> Steppers;
	/** The setup calendar's rows, one per round, each opening its rules. */
	UPROPERTY(Transient) TArray<TObjectPtr<UApexButtonWidget>> RoundButtons;
	UPROPERTY(Transient) TObjectPtr<UScrollBox> CalendarScroll;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> RoundDoneButton;
	UPROPERTY(Transient) TObjectPtr<UApexButtonWidget> RoundResetButton;

	/** Classes (catalog CarClass) with their cars' ids, and the circuits, sorted by name. */
	TArray<FString> Classes;
	TMap<FString, TArray<FString>> CarsByClass;
	TArray<FString> TrackIds;

	/** The setup's choices, kept while the screen is away. */
	int32 ClassIndex = 0;
	int32 CarIndex = 0;
	int32 RoundCount = 6;
	int32 PointsIndex = 0;
	int32 Laps = 8;
	int32 AiCount = 11;
	int32 AiSkill = -1;
	int32 DropRounds = 0;
	/** EApexSuccessBallast. */
	int32 BallastIndex = 0;
	int32 FastestLapPoints = 0;
	bool bFastestLapNeedsPoints = true;
	/** Each round's own rules by place in the calendar (only the rules are read); kept when the round count changes. */
	TArray<FApexSeasonRound> RoundRules;
	/** The round whose rules the right column shows; INDEX_NONE for the calendar. */
	int32 EditingRound = INDEX_NONE;

	/** Round 1's circuit as `apexsim.season.Setup` named it; empty for the spread's. */
	FString FirstTrackOverride;

	/** End season is pressed twice. */
	bool bEndArmed = false;
	bool bBound = false;
};
