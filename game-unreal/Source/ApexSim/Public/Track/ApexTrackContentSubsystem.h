#pragma once

#include "CoreMinimal.h"
#include "Catalog/ApexCatalogRows.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Track/ApexTrackInstance.h"

#include "ApexTrackContentSubsystem.generated.h"

class UTexture2D;
class UWorld;

/**
 * Where the game's circuits come from, and the one place that decides it.
 *
 * Two kinds of track: a cooked level (`/Game/Tracks/<Stem>/L_<Stem>`, made
 * by the editor's `ApexTrackImport`) and a runtime track (`<Stem>.uescene.json`
 * + `<Stem>.uemesh` from `ats-export`, found on disk at startup and built in
 * the running game). A new circuit needs only its export dropped in the
 * track folder — no editor, no cook, no repackage.
 *
 * The track folders, in order:
 *  - `-ApexTracksDir=<dir>` on the command line (repeatable with `+`);
 *  - a packaged build: `Tracks/` beside `ApexSim.exe` (`<Release>/Game/Tracks`);
 *  - the editor: the repo's `content/tracks/export`.
 * A track's preview is `<Stem>.png` beside its manifest or under `previews/`.
 *
 * Which kind a track loads as (`apexsim.track.Source`, or `-ApexTrackSource=`):
 *  - `auto` (default): the cooked level when there is one, else the export;
 *  - `runtime`: the export when there is one, else the cooked level;
 *  - `cooked`: cooked levels only.
 * The catalog follows the same choice, so the checksum compared with the
 * server's is the one of the file the track on screen was built from.
 *
 * A runtime track released by the race director is kept, hidden, and handed
 * back when the same circuit is asked for next (the demo behind the menu,
 * then the player's race on it), unless `apexsim.track.KeepLast 0`.
 */
UCLASS()
class APEXSIM_API UApexTrackContentSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Scan the track folders again (`apexsim.track.Rescan`). */
	void Rescan();

	/** The folders scanned, in order. */
	static TArray<FString> TrackDirectories();

	/** `/Game/Tracks/<Stem>/L_<Stem>`. */
	static FString CookedLevelPath(const FString& Stem);
	static bool HasCookedLevel(const FString& Stem);

	/** How `Stem` would be loaded right now. */
	EApexTrackSource ResolveSource(const FString& Stem) const;
	/** Whether there is anything to race on for `Stem`. */
	bool HasTrack(const FString& Stem) const { return ResolveSource(Stem) != EApexTrackSource::None; }

	const FApexRuntimeTrackFiles* FindRuntimeTrack(const FString& Stem) const;
	int32 NumRuntimeTracks() const { return RuntimeTracks.Num(); }

	/**
	 * The catalog row made from a runtime track's manifest, by `track_id`;
	 * null when no export has that id. Whether it or the data table's row
	 * wins is `UseRuntimeRow`.
	 */
	const FApexTrackCatalogRow* FindRuntimeRow(const FString& TrackId) const;
	/** True when the catalog should describe `TrackId` from its runtime export rather than the table. */
	bool UseRuntimeRow(const FString& TrackId, bool bHasTableRow) const;
	/** Row id of the runtime track whose stem is `Stem`, or empty. */
	FString FindRuntimeTrackIdByStem(const FString& Stem) const;

	/**
	 * Start bringing `Stem` into `World`, or hand back the one kept from
	 * last time. Null when there is nothing to load. The caller gives it
	 * back with `Release`.
	 */
	UApexTrackInstance* Acquire(UWorld* World, const FString& Stem);
	void Release(UApexTrackInstance* Instance);

	/** The preview a catalog row shows: its runtime PNG, else the table's texture. */
	static UTexture2D* PreviewOf(const FApexTrackCatalogRow& Row);

private:
	void DropKept();

	/** Stem (lower case) -> files. */
	TMap<FString, FApexRuntimeTrackFiles> RuntimeTracks;
	/** Track id (lower case) -> row. */
	TMap<FString, FApexTrackCatalogRow> RuntimeRows;

	/** The runtime previews, kept alive for the rows that point at them. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UTexture2D>> Previews;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UApexTrackInstance>> Live;

	/** A released runtime track, hidden, for the next `Acquire` of the same circuit. */
	UPROPERTY(Transient)
	TObjectPtr<UApexTrackInstance> Kept;
};
