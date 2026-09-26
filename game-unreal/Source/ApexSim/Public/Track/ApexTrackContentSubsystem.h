#pragma once

#include "CoreMinimal.h"
#include "Catalog/ApexCatalogRows.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Track/ApexTrackInstance.h"

#include "ApexTrackContentSubsystem.generated.h"

class UTexture2D;
class UWorld;

/**
 * Where the game's circuits come from: their exports on disk.
 *
 * Every circuit is built at runtime from `<Stem>.uescene.json` +
 * `<Stem>.uemesh` (from `ats-export`), found in the track folders at
 * startup; there are no cooked track levels. A new circuit needs only its
 * export (and a preview) dropped in the track folder — no editor, no cook,
 * no repackage.
 *
 * The track folders, in order:
 *  - `-ApexTracksDir=<dir>` on the command line (several joined with `+`);
 *  - a packaged build: `Tracks/` beside `ApexSim.exe` (`<Release>/Game/Tracks`);
 *  - the editor: the repo's `content/tracks/export`.
 * A track's preview is `<Stem>.png` beside its manifest or under `previews/`.
 *
 * Each export's manifest head becomes a catalog row (`FindRuntimeRow`),
 * which wins over the `DT_TrackCatalog` table's; the table is only a
 * fallback for names and art of tracks with no export on this machine.
 *
 * A track released by the race director is kept, hidden, and handed back
 * when the same circuit is asked for next (the demo behind the menu, then
 * the player's race on it), unless `apexsim.track.KeepLast 0`.
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

	/**
	 * Whether there is an export to build `Stem` from, and it has not
	 * already failed to build in this run.
	 */
	bool HasTrack(const FString& Stem) const
	{
		return FindRuntimeTrack(Stem) != nullptr && !BrokenStems.Contains(Stem.ToLower());
	}

	const FApexRuntimeTrackFiles* FindRuntimeTrack(const FString& Stem) const;
	int32 NumRuntimeTracks() const { return RuntimeTracks.Num(); }

	/**
	 * The catalog row made from a track's manifest, by `track_id`; null
	 * when no export has that id.
	 */
	const FApexTrackCatalogRow* FindRuntimeRow(const FString& TrackId) const;
	/** Row id of the runtime track whose stem is `Stem`, or empty. */
	FString FindRuntimeTrackIdByStem(const FString& Stem) const;

	/**
	 * Start building `Stem` into `World`, or hand back the one kept from
	 * last time. Null when there is no export for it. The caller gives it
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
	/** Stems (lower case) whose build failed this run; cleared by a rescan. */
	TSet<FString> BrokenStems;

	/** The runtime previews, kept alive for the rows that point at them. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UTexture2D>> Previews;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UApexTrackInstance>> Live;

	/** A released runtime track, hidden, for the next `Acquire` of the same circuit. */
	UPROPERTY(Transient)
	TObjectPtr<UApexTrackInstance> Kept;
};
