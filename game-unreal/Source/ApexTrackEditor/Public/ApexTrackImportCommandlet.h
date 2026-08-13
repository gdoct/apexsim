#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "ApexTrackImportCommandlet.generated.h"

struct FApexTrackScene;

/**
 * Turns baked `.uescene.json` track exports into levels.
 *
 * ```
 * ApexSimEditor-Cmd.exe <uproject> -run=ApexTrackImport -track=Monza
 * ApexSimEditor-Cmd.exe <uproject> -run=ApexTrackImport -all
 * ```
 *
 * Options:
 *   -track=NAME    one track by export stem (repeatable, or comma-separated)
 *   -all           every export in the source directory
 *   -source=DIR    where the exports live (default: <project>/../content/tracks/export)
 *   -dest=PATH     content root for generated assets (default: /Game/Tracks)
 *   -dryrun        parse and report, write nothing
 *
 * Everything it writes lives under `-dest` and is regenerated wholesale on
 * the next run, so nothing in there should be hand-edited.
 */
UCLASS()
class APEXTRACKEDITOR_API UApexTrackImportCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UApexTrackImportCommandlet();

	virtual int32 Main(const FString& Params) override;

private:
	/** Resolved from the command line in `Main`. */
	struct FOptions
	{
		TArray<FString> TrackNames;
		bool bAll = false;
		FString SourceDir;
		FString DestRoot;
		bool bDryRun = false;
	};

	static bool ParseOptions(const FString& Params, FOptions& Out, FString& OutError);

	/** Default export directory, derived from the project location. */
	static FString DefaultSourceDir();

	/** Exports to process, given the options. Sorted, for reproducible runs. */
	static bool CollectExports(const FOptions& Options, TArray<FString>& OutPaths, FString& OutError);

	/** Import one export. Returns false if the track was not fully written. */
	bool ImportTrack(const FString& ExportPath, const FOptions& Options);

	/** Log what an export contains, without writing anything. */
	static void ReportScene(const FApexTrackScene& Scene, const FString& ExportPath);
};
