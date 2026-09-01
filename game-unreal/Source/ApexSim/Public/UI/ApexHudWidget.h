#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "ApexSettingsSave.h"
#include "ApexSettingsSubsystem.h"
#include "Blueprint/UserWidget.h"
#include "Containers/StaticArray.h"

#include "ApexHudWidget.generated.h"

class UApexMenuFlowSubsystem;
class UApexMinimapWidget;
class UApexNetSubsystem;
class UApexSettingsSubsystem;
class UBorder;
class UHorizontalBox;
class UHorizontalBoxSlot;
class UProgressBar;
class USizeBox;
class UTextBlock;
class UVerticalBox;
class UWidget;

/**
 * The race HUD.
 *
 * Everything on it is derived, because the protocol carries no HUD: telemetry
 * gives each car a lap number, a fraction of a lap, a lap time and a speed, and
 * position, gaps, the delta and the standings all fall out of those. Nothing
 * here asks the server for anything it does not already broadcast.
 *
 * Built in C++ like the rest of the shell and rebuilt only when the detail
 * level changes; the per-frame path just sets text on widgets that already
 * exist.
 */
UCLASS()
class APEXSIM_API UApexHudWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UApexHudWidget(const FObjectInitializer& ObjectInitializer);

	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

	/** Shows or hides the whole HUD, and starts or stops its per-frame work. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|UI")
	void SetRaceActive(bool bActive);

protected:
	UFUNCTION()
	void HandleSettingsChanged(EApexSettingsGroup Group);

	UFUNCTION()
	void HandleTelemetry(const FApexTelemetryFrame& Frame);

private:
	// --- Construction ---------------------------------------------------------

	/** Rebuilds the whole tree for the current detail level. */
	void BuildHud();
	UWidget* BuildTopBar();
	UWidget* BuildRaceStateStrip();
	UWidget* BuildStandingsPanel();
	UWidget* BuildDeltaPanel();
	UWidget* BuildPedalPanel();
	UWidget* BuildCarStatePanel();
	UWidget* BuildMinimapPanel();

	// --- Per-frame ------------------------------------------------------------

	void RefreshHeader();
	void RefreshRaceState();
	void RefreshStandings();
	void RefreshCarState();
	void RefreshDelta();
	void RefreshMinimap();

	/** One car's classification, sorted best-first. */
	struct FStanding
	{
		int32 CarIndex = 0;
		FString Name;
		/** Race distance in metres; negative on the grid behind the line. */
		float Progress = 0.0f;
		float SpeedMps = 0.0f;
		bool bIsLocal = false;
	};

	/** Field order this frame, leader first. */
	void ComputeStandings(TArray<FStanding>& OutOrder) const;

	/** The local player's telemetry this frame, or null. */
	const FApexCarTelemetry* FindLocalCar() const;

	/** The circuit's length from the track catalog, or 0 when unknown. */
	float CatalogTrackLengthM() const;

	/**
	 * Samples the lap in progress and, when it turns out to be the fastest,
	 * keeps it as the reference the delta is measured against.
	 *
	 * A delta needs a lap to compare with, and the protocol only ever reports
	 * one number — the elapsed time of the current lap. Recording elapsed time
	 * against track position turns that into a curve, and the difference
	 * between the live curve and the reference is the delta a driver expects.
	 */
	void UpdateDeltaReference(const FApexCarTelemetry& Local);

	/** Reference-lap time at a fraction of the lap, or -1 with no reference. */
	float ReferenceTimeAt(float Progress) const;

	/** Length of one sector, from the cumulative splits. Zero if unfinished. */
	static float SectorDuration(const TStaticArray<float, 3>& Splits, int32 Index);

	FString FormatSpeed(float Mps) const;
	static FString FormatGap(float Seconds, bool bSigned = true);

	UApexNetSubsystem* GetNet() const;
	UApexSettingsSubsystem* GetSettings() const;
	UApexMenuFlowSubsystem* GetFlow() const;

	// --- Widgets --------------------------------------------------------------

	UPROPERTY(Transient) TObjectPtr<UTextBlock> TrackNameText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SessionLineText;

	UPROPERTY(Transient) TObjectPtr<UTextBlock> PositionText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> PositionOfText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> LapText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> LapOfText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> AheadLabel;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> AheadValue;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> BehindLabel;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> BehindValue;

	UPROPERTY(Transient) TObjectPtr<UBorder> PingDot;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> PingText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> CarCountText;

	/** Five rows: place, name, time. Hidden from the size box, painted on the border. */
	UPROPERTY(Transient) TArray<TObjectPtr<USizeBox>> StandingSlots;
	UPROPERTY(Transient) TArray<TObjectPtr<UBorder>> StandingRows;
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> StandingPlace;
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> StandingName;
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> StandingTime;

	UPROPERTY(Transient) TObjectPtr<UTextBlock> DeltaValue;
	UPROPERTY(Transient) TArray<TObjectPtr<UBorder>> SectorBars;

	UPROPERTY(Transient) TObjectPtr<UProgressBar> ThrottleBar;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> BrakeBar;

	/** One border per RPM segment, lit left to right. */
	UPROPERTY(Transient) TArray<TObjectPtr<UBorder>> RpmSegments;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> RpmText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> GearText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SpeedText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SpeedUnitText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> LastLapText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> BestLapText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> LapsLeftText;

	UPROPERTY(Transient) TObjectPtr<UApexMinimapWidget> Minimap;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SectorCaption;

	UPROPERTY(Transient) TObjectPtr<UVerticalBox> RootStack;

	// --- Derived state --------------------------------------------------------

	/** Elapsed lap time sampled against track position, for the current lap. */
	TArray<TPair<float, float>> LapSamples;
	/** The same curve for the fastest lap so far; empty until one completes. */
	TArray<TPair<float, float>> ReferenceLap;

	/**
	 * Cumulative sector splits — the elapsed lap time as each third went past —
	 * for the lap in progress and for the reference lap. Zero means unfinished.
	 */
	TStaticArray<float, 3> SectorSplits;
	TStaticArray<float, 3> ReferenceSplits;

	/** The mode the header was last written for, so it is rebuilt when it moves. */
	EApexGameMode HeaderGameMode = EApexGameMode::Lobby;

	int32 LastSeenLap = 0;
	float LastLapSeconds = 0.0f;
	float BestLapSeconds = 0.0f;

	/** Highest RPM seen this session — the protocol never states a redline. */
	float ObservedMaxRpm = 8000.0f;

	/** What the tree was last built for, so a settings change rebuilds once. */
	EApexHudDetail BuiltDetail = EApexHudDetail::All;
	bool bRaceActive = false;
};
