#pragma once

#include "CoreMinimal.h"
#include "Hud/ApexHudData.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "ApexHudDataSubsystem.generated.h"

struct FApexMinimapBlip;
class UApexNetSubsystem;

/**
 * The game's side of the HUD: gathers the session, the telemetry, the timing
 * board and the settings into FApexHudData once a frame while a race view is
 * open. The HUD widget draws the components from it; anything else (a
 * Blueprint widget, a future script host, an overlay app) reads the same data
 * points through GetNumber / GetText / GetBool or GetData.
 *
 * `apexsim.hud.Data [filter]` prints every data point and its value.
 */
UCLASS()
class APEXSIM_API UApexHudDataSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** A race view opened or closed: a new race starts with no reference lap, fuel history or rev scale. */
	void SetRaceActive(bool bActive);
	bool IsRaceActive() const { return bRaceActive; }

	/** Rebuild the data from the game's state; the HUD calls it once a frame while it is shown. */
	void Refresh();

	/**
	 * Fill the data from a made-up race (FApexHudPreview) whenever no real one
	 * is running: the HUD editor, opened from the main menu, lays the panels
	 * out over it.
	 */
	void SetPreview(bool bInPreview);
	bool IsShowingPreview() const { return bPreview && !bRaceActive; }

	const FApexHudData& GetData() const { return Data; }

	/** A data point as a number (null reads 0). */
	UFUNCTION(BlueprintPure, Category = "ApexSim|HUD")
	float GetNumber(FName Name) const;

	/** A data point as text, the way a component's `{name}` would print it. */
	UFUNCTION(BlueprintPure, Category = "ApexSim|HUD")
	FString GetText(FName Name) const;

	UFUNCTION(BlueprintPure, Category = "ApexSim|HUD")
	bool GetBool(FName Name) const;

	/** Whether the game filled the data point this frame (false for null and for an unknown name). */
	UFUNCTION(BlueprintPure, Category = "ApexSim|HUD")
	bool HasValue(FName Name) const;

	/** One line per data point and list row, sorted; Filter keeps the lines containing it. */
	TArray<FString> DescribeData(const FString& Filter = FString()) const;

	/** The current circuit's outline for the minimap, empty until the lobby state carries it. */
	const TArray<FVector2D>& GetTrackOutline();

	/** Every car on the map, the local one marked. */
	void MakeMinimapBlips(TArray<FApexMinimapBlip>& Out, const FLinearColor& LocalColour) const;

private:
	UFUNCTION()
	void HandleTelemetry(const FApexTelemetryFrame& Frame);

	UApexNetSubsystem* GetNet() const;
	float CatalogTrackLengthM() const;

	FApexHudData Data;
	FApexHudMemory Memory;
	bool bPreview = false;
	TUniquePtr<FApexHudPreview> Preview;
	/** The frame the data was last built from, for the minimap's blips. */
	const FApexTelemetryFrame* LastFrame = nullptr;
	int32 LastLocalIndex = -1;
	bool bRaceActive = false;
	double RaceStartSeconds = 0.0;

	/** The car the catalog figures below were read for. */
	FString CatalogCarId;
	float TyreOptimalC = 90.0f;
	float TyreWindowC = 10.0f;
	float RedlineRpm = 0.0f;
	float LimiterRpm = 0.0f;
	FString CarName;

	TArray<FVector2D> TrackOutline;
	FString TrackOutlineId;
};
