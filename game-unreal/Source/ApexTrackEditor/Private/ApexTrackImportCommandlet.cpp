#include "ApexTrackImportCommandlet.h"

#include "ApexTrackAssetBuilder.h"
#include "ApexTrackEditorModule.h"
#include "ApexTrackSceneData.h"
#include "ApexTrackSceneReader.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

namespace
{
	/** Exports carry a compound extension, so `GetBaseFilename` is not enough. */
	FString ExportStem(const FString& Path)
	{
		FString Stem = FPaths::GetCleanFilename(Path);
		Stem.RemoveFromEnd(TEXT(".uescene.json"), ESearchCase::IgnoreCase);
		return Stem;
	}
}	 // namespace

UApexTrackImportCommandlet::UApexTrackImportCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
}

FString UApexTrackImportCommandlet::DefaultSourceDir()
{
	// The repo layout puts the Unreal project next to `content/`, and the
	// exports are gitignored build output rather than shipped content.
	return FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectDir(), TEXT("../content/tracks/export")));
}

bool UApexTrackImportCommandlet::ParseOptions(
	const FString& Params, FOptions& Out, FString& OutError)
{
	TArray<FString> Tokens;
	TArray<FString> Switches;
	TMap<FString, FString> Values;
	ParseCommandLine(*Params, Tokens, Switches, Values);

	Out.bAll = Switches.Contains(TEXT("all"));
	Out.bDryRun = Switches.Contains(TEXT("dryrun"));

	if (const FString* Source = Values.Find(TEXT("source")))
	{
		Out.SourceDir = FPaths::ConvertRelativePathToFull(*Source);
	}
	else
	{
		Out.SourceDir = DefaultSourceDir();
	}

	Out.DestRoot = Values.Contains(TEXT("dest")) ? Values[TEXT("dest")] : TEXT("/Game/Tracks");
	if (!Out.DestRoot.StartsWith(TEXT("/Game")))
	{
		OutError = FString::Printf(
			TEXT("-dest must be a content path starting with /Game, got \"%s\""), *Out.DestRoot);
		return false;
	}
	Out.DestRoot.RemoveFromEnd(TEXT("/"));

	if (const FString* Track = Values.Find(TEXT("track")))
	{
		Track->ParseIntoArray(Out.TrackNames, TEXT(","), true);
		for (FString& Name : Out.TrackNames)
		{
			Name.TrimStartAndEndInline();
		}
	}

	if (!Out.bAll && Out.TrackNames.IsEmpty())
	{
		OutError = TEXT("nothing to do: pass -track=NAME or -all");
		return false;
	}
	return true;
}

bool UApexTrackImportCommandlet::CollectExports(
	const FOptions& Options, TArray<FString>& OutPaths, FString& OutError)
{
	if (!IFileManager::Get().DirectoryExists(*Options.SourceDir))
	{
		OutError = FString::Printf(
			TEXT("no export directory at %s — bake the tracks first with `cargo run --bin "
				 "ats-export -- --all` in track-editor/"),
			*Options.SourceDir);
		return false;
	}

	TArray<FString> Found;
	IFileManager::Get().FindFiles(Found, *(Options.SourceDir / TEXT("*.uescene.json")), true, false);
	Found.Sort();

	if (Options.bAll)
	{
		for (const FString& File : Found)
		{
			OutPaths.Add(Options.SourceDir / File);
		}
	}

	for (const FString& Name : Options.TrackNames)
	{
		const FString* Match = Found.FindByPredicate(
			[&Name](const FString& File) { return ExportStem(File).Equals(Name, ESearchCase::IgnoreCase); });
		if (!Match)
		{
			OutError = FString::Printf(
				TEXT("no export named \"%s\" in %s — bake it first"), *Name, *Options.SourceDir);
			return false;
		}
		OutPaths.AddUnique(Options.SourceDir / *Match);
	}

	if (OutPaths.IsEmpty())
	{
		OutError = FString::Printf(TEXT("%s contains no .uescene.json exports"), *Options.SourceDir);
		return false;
	}
	return true;
}

void UApexTrackImportCommandlet::ReportScene(const FApexTrackScene& Scene, const FString& ExportPath)
{
	int32 Vertices = 0;
	int32 Triangles = 0;
	for (const FApexTrackMesh& Mesh : Scene.Meshes)
	{
		Vertices += Mesh.Positions.Num();
		Triangles += Mesh.NumTriangles();
	}

	UE_LOG(LogApexTrackImport, Display, TEXT("%s"), *ExportStem(ExportPath));
	UE_LOG(LogApexTrackImport, Display, TEXT("    \"%s\" (%s), %.0f m%s"), *Scene.TrackName,
		*Scene.Country, Scene.LengthCm / 100.0f, Scene.bClosedLoop ? TEXT(", closed loop") : TEXT(""));
	UE_LOG(LogApexTrackImport, Display,
		TEXT("    %d mesh(es), %d vertices, %d triangles, %d material(s)"), Scene.Meshes.Num(),
		Vertices, Triangles, Scene.Materials.Num());
	UE_LOG(LogApexTrackImport, Display,
		TEXT("    %d prop(s), %d grid slot(s), %d centerline point(s)%s"), Scene.Props.Num(),
		Scene.Grid.Num(), Scene.Centerline.Num(),
		Scene.PitLane.IsSet() ? TEXT(", pit lane") : TEXT(""));
}

bool UApexTrackImportCommandlet::ImportTrack(const FString& ExportPath, const FOptions& Options)
{
	FApexTrackScene Scene;
	FString Error;
	if (!FApexTrackSceneReader::LoadFromFile(ExportPath, Scene, Error))
	{
		UE_LOG(LogApexTrackImport, Error, TEXT("%s"), *Error);
		return false;
	}

	ReportScene(Scene, ExportPath);

	if (Options.bDryRun)
	{
		return true;
	}

	FApexTrackAssetBuilder Builder(Options.DestRoot, ExportStem(ExportPath));
	if (!Builder.Build(Scene, Error))
	{
		UE_LOG(LogApexTrackImport, Error, TEXT("    %s"), *Error);
		return false;
	}
	return true;
}

int32 UApexTrackImportCommandlet::Main(const FString& Params)
{
	FOptions Options;
	FString Error;
	if (!ParseOptions(Params, Options, Error))
	{
		UE_LOG(LogApexTrackImport, Error, TEXT("%s"), *Error);
		return 1;
	}

	TArray<FString> Exports;
	if (!CollectExports(Options, Exports, Error))
	{
		UE_LOG(LogApexTrackImport, Error, TEXT("%s"), *Error);
		return 1;
	}

	UE_LOG(LogApexTrackImport, Display, TEXT("Importing %d track(s) from %s into %s%s"),
		Exports.Num(), *Options.SourceDir, *Options.DestRoot,
		Options.bDryRun ? TEXT(" (dry run)") : TEXT(""));

	int32 Failures = 0;
	for (const FString& ExportPath : Exports)
	{
		if (!ImportTrack(ExportPath, Options))
		{
			++Failures;
		}
	}

	UE_LOG(LogApexTrackImport, Display, TEXT("Imported %d/%d track(s)"),
		Exports.Num() - Failures, Exports.Num());
	return Failures > 0 ? 1 : 0;
}
