#pragma once

#include "CoreMinimal.h"
#include "Season/ApexSeason.h"
#include "UI/ApexScreenWidget.h"

#include "ApexSeasonWidget.generated.h"

class UApexButtonWidget;
class UApexStepperWidget;
class UTextBlock;
class UVerticalBox;

/**
 * An offline season against the AI (Season/ApexSeason.h, saved in the
 * driver's profile). Without one: a setup card (class, car, rounds, points,
 * laps, field, AI level, dropped rounds) and the calendar it makes. With
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
	virtual bool WantsLiveBackdrop() const override { return false; }

	/** The setup's laps, field and rounds, as `apexsim.season.Setup` sets them. */
	void SetSetup(int32 InLaps, int32 InAi, int32 InRounds, const FString& FirstStem = FString());
	/** The main button, as `apexsim.season.Race` presses it. */
	void PressPrimary();

protected:
	virtual void NativeOnInitialized() override;

private:
	void BuildLayout();
	void Rebuild();
	void RebuildSetup();
	void RebuildActive();
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

	/** Round 1's circuit as `apexsim.season.Setup` named it; empty for the spread's. */
	FString FirstTrackOverride;

	/** End season is pressed twice. */
	bool bEndArmed = false;
	bool bBound = false;
};
