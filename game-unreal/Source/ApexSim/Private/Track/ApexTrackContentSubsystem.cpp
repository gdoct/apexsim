#include "Track/ApexTrackContentSubsystem.h"

#include "ApexSim.h"
#include "Engine/DataTable.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "ImageUtils.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Track/ApexTrackSceneBuilder.h"
#include "Track/ApexTrackSceneReader.h"

namespace
{
	TAutoConsoleVariable<bool> CVarTrackKeepLast(TEXT("apexsim.track.KeepLast"), true,
		TEXT("Keep the last runtime-built track, hidden, to hand back when the same circuit is loaded next."));

	FAutoConsoleCommandWithWorld RescanCommand(TEXT("apexsim.track.Rescan"),
		TEXT("Scan the track folders for runtime track exports again."),
		FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World) {
			UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
			if (UApexTrackContentSubsystem* Content = GameInstance ? GameInstance->GetSubsystem<UApexTrackContentSubsystem>() : nullptr)
			{
				Content->Rescan();
			}
		}));

	const TCHAR* kRuntimeTrackTablePath = TEXT("/Game/Data/DT_TrackCatalog.DT_TrackCatalog");

	/** The preview beside a manifest, or under `previews/` there; empty if neither exists. */
	FString FindPreview(const FString& Dir, const FString& Stem)
	{
		for (const FString& Candidate :
			{FPaths::Combine(Dir, Stem + TEXT(".png")), FPaths::Combine(Dir, TEXT("previews"), Stem + TEXT(".png"))})
		{
			if (IFileManager::Get().FileExists(*Candidate))
			{
				return Candidate;
			}
		}
		return FString();
	}
}	 // namespace

TArray<FString> UApexTrackContentSubsystem::TrackDirectories()
{
	TArray<FString> Dirs;
	FString Override;
	if (FParse::Value(FCommandLine::Get(), TEXT("-ApexTracksDir="), Override))
	{
		TArray<FString> Parts;
		Override.ParseIntoArray(Parts, TEXT("+"), true);
		for (const FString& Part : Parts)
		{
			Dirs.Add(FPaths::ConvertRelativePathToFull(Part));
		}
	}
	// In a packaged build ProjectDir is <Release>/Game/ApexSim/, so its parent
	// is the folder holding ApexSim.exe and settings.yml; the tracks sit in
	// `Tracks/` beside them. In the editor the exports never leave the repo.
	const FString Default = FPlatformProperties::RequiresCookedData()
		? FPaths::Combine(FPaths::ProjectDir(), TEXT(".."), TEXT("Tracks"))
		: FPaths::Combine(FPaths::ProjectDir(), TEXT(".."), TEXT("content"), TEXT("tracks"), TEXT("export"));
	Dirs.AddUnique(FPaths::ConvertRelativePathToFull(Default));
	return Dirs;
}

void UApexTrackContentSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Rescan();
}

void UApexTrackContentSubsystem::Deinitialize()
{
	DropKept();
	for (UApexTrackInstance* Instance : Live)
	{
		if (Instance)
		{
			Instance->Unload();
		}
	}
	Live.Reset();
	Super::Deinitialize();
}

void UApexTrackContentSubsystem::Rescan()
{
	// A kept track was built from the files as they were; a rescan is how a
	// re-exported circuit is picked up, so it must not be handed back.
	DropKept();
	RuntimeTracks.Reset();
	RuntimeRows.Reset();
	BrokenStems.Reset();
	Previews.Reset();

	// Table previews for tracks that ship without a PNG of their own. Tested
	// first: a fresh clone has no table, which is no reason for an error.
	const FString TablePackage = FPackageName::ObjectPathToPackageName(FString(kRuntimeTrackTablePath));
	const UDataTable* Table = FPackageName::DoesPackageExist(TablePackage)
		? LoadObject<UDataTable>(nullptr, kRuntimeTrackTablePath)
		: nullptr;

	int32 Skipped = 0;
	for (const FString& Dir : TrackDirectories())
	{
		if (!IFileManager::Get().DirectoryExists(*Dir))
		{
			UE_LOG(LogApexSim, Log, TEXT("Runtime tracks: no folder at %s"), *Dir);
			continue;
		}
		TArray<FString> Found;
		IFileManager::Get().FindFiles(
			Found, *FPaths::Combine(Dir, FString(TEXT("*")) + FApexTrackSceneReader::SceneExtension()), true, false);
		Found.Sort();
		for (const FString& File : Found)
		{
			const FString Path = FPaths::Combine(Dir, File);
			FApexRuntimeTrackFiles Files;
			FString Error;
			if (!FApexTrackSceneReader::LoadHeader(Path, Files.Header, Error))
			{
				UE_LOG(LogApexSim, Warning, TEXT("Runtime tracks: skipping %s: %s"), *Path, *Error);
				++Skipped;
				continue;
			}
			Files.Stem = FApexTrackSceneReader::StemOf(Path);
			Files.ScenePath = Path;
			Files.PreviewPath = FindPreview(Dir, Files.Stem);
			const FString StemKey = Files.Stem.ToLower();
			if (RuntimeTracks.Contains(StemKey))
			{
				// An earlier folder wins: that is what -ApexTracksDir is for.
				continue;
			}
			if (!Files.Header.MeshBlob.IsEmpty()
				&& !IFileManager::Get().FileExists(*FPaths::Combine(Dir, Files.Header.MeshBlob)))
			{
				UE_LOG(LogApexSim, Warning, TEXT("Runtime tracks: %s names %s, which is not there; skipped"), *Path,
					*Files.Header.MeshBlob);
				++Skipped;
				continue;
			}

			if (!Files.Header.TrackId.IsEmpty())
			{
				FApexTrackCatalogRow Row;
				// Never the real name when there is a substitute: `track_name`
				// is the circuit's trademark, `track_display_name` the parody.
				Row.DisplayName = Files.Header.DisplayName.IsEmpty() ? Files.Header.TrackName : Files.Header.DisplayName;
				Row.Description = Files.Header.Description;
				Row.Country = Files.Header.Country;
				Row.City = Files.Header.City;
				Row.Category = Files.Header.Category;
				Row.EnvironmentType = Files.Header.EnvironmentType;
				Row.LengthM = Files.Header.LengthCm / 100.0f;
				Row.YamlBaseName = Files.Stem;
				Row.SourceCrc = Files.Header.SourceCrc;
				if (!Files.PreviewPath.IsEmpty())
				{
					Row.RuntimePreview = FImageUtils::ImportFileAsTexture2D(Files.PreviewPath);
					if (Row.RuntimePreview)
					{
						Previews.Add(Row.RuntimePreview);
					}
				}
				if (!Row.RuntimePreview && Table)
				{
					for (const TPair<FName, uint8*>& Pair : Table->GetRowMap())
					{
						if (Pair.Key.ToString().Equals(Files.Header.TrackId, ESearchCase::IgnoreCase))
						{
							Row.PreviewImage = reinterpret_cast<const FApexTrackCatalogRow*>(Pair.Value)->PreviewImage;
							break;
						}
					}
				}
				RuntimeRows.Add(Files.Header.TrackId.ToLower(), MoveTemp(Row));
			}
			else
			{
				UE_LOG(LogApexSim, Warning,
					TEXT("Runtime tracks: %s has no track_id, so no catalog row can match it; it loads only by name"),
					*Path);
			}
			RuntimeTracks.Add(StemKey, MoveTemp(Files));
		}
	}

	TArray<FString> Stems;
	for (const TPair<FString, FApexRuntimeTrackFiles>& Pair : RuntimeTracks)
	{
		Stems.Add(Pair.Value.Stem);
	}
	Stems.Sort();
	UE_LOG(LogApexSim, Log, TEXT("Runtime tracks: %d found (%s)%s"), Stems.Num(),
		*FString::Join(Stems, TEXT(", ")), Skipped > 0 ? *FString::Printf(TEXT(", %d skipped"), Skipped) : TEXT(""));
}

const FApexRuntimeTrackFiles* UApexTrackContentSubsystem::FindRuntimeTrack(const FString& Stem) const
{
	return RuntimeTracks.Find(Stem.ToLower());
}

const FApexTrackCatalogRow* UApexTrackContentSubsystem::FindRuntimeRow(const FString& TrackId) const
{
	return TrackId.IsEmpty() ? nullptr : RuntimeRows.Find(TrackId.ToLower());
}

FString UApexTrackContentSubsystem::FindRuntimeTrackIdByStem(const FString& Stem) const
{
	if (const FApexRuntimeTrackFiles* Files = FindRuntimeTrack(Stem))
	{
		return Files->Header.TrackId;
	}
	return FString();
}

UApexTrackInstance* UApexTrackContentSubsystem::Acquire(UWorld* World, const FString& Stem)
{
	const FApexRuntimeTrackFiles* Files = FindRuntimeTrack(Stem);
	if (!World || !Files)
	{
		return nullptr;
	}
	if (Kept && Kept->GetWorld() == World && Kept->GetStem().Equals(Stem, ESearchCase::IgnoreCase) && Kept->IsLoaded())
	{
		UApexTrackInstance* Reused = Kept;
		Kept = nullptr;
		Reused->ResetForReuse();
		Live.Add(Reused);
		return Reused;
	}
	// One circuit in the world at a time: another's gantry and road would
	// answer the director's tag searches and traces.
	DropKept();

	UApexTrackInstance* Instance = NewObject<UApexTrackInstance>(this);
	if (!Instance->Start(World, *Files))
	{
		Instance->Unload();
		return nullptr;
	}
	Live.Add(Instance);
	return Instance;
}

void UApexTrackContentSubsystem::Release(UApexTrackInstance* Instance)
{
	if (!Instance)
	{
		return;
	}
	Live.Remove(Instance);
	if (Instance->HasFailed())
	{
		BrokenStems.Add(Instance->GetStem().ToLower());
	}
	if (Instance->IsLoaded() && CVarTrackKeepLast.GetValueOnGameThread())
	{
		DropKept();
		Instance->SetVisible(false);
		Kept = Instance;
		return;
	}
	Instance->Unload();
}

void UApexTrackContentSubsystem::DropKept()
{
	if (Kept)
	{
		Kept->Unload();
		Kept = nullptr;
	}
}

UTexture2D* UApexTrackContentSubsystem::PreviewOf(const FApexTrackCatalogRow& Row)
{
	if (Row.RuntimePreview)
	{
		return Row.RuntimePreview;
	}
	return Row.PreviewImage.IsNull() ? nullptr : Row.PreviewImage.LoadSynchronous();
}
