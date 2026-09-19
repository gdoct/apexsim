#include "ApexGroundTexImportCommandlet.h"

#include "ApexGroundMaterials.h"
#include "ApexTrackEditorModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
	/** The three maps a set is baked as, and how each is to be read. */
	struct FMapKind
	{
		const TCHAR* Suffix;
		/** Colour maps are authored in sRGB; the other two are data. */
		bool bSRGB;
		TextureCompressionSettings Compression;
		TextureGroup Group;
	};

	const FMapKind kMapKinds[] = {
		{TEXT("col"), true, TC_Default, TEXTUREGROUP_World},
		{TEXT("nrm"), false, TC_Normalmap, TEXTUREGROUP_WorldNormalMap},
		// One channel, so BC4 rather than a full RGBA block. The material
		// reads red, which is what a grayscale sampler gives it.
		{TEXT("rough"), false, TC_Grayscale, TEXTUREGROUP_World},
	};

	const FMapKind* FindMapKind(const FString& Suffix)
	{
		for (const FMapKind& Kind : kMapKinds)
		{
			if (Suffix == Kind.Suffix)
			{
				return &Kind;
			}
		}
		return nullptr;
	}
}	 // namespace

UApexGroundTexImportCommandlet::UApexGroundTexImportCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
}

FString UApexGroundTexImportCommandlet::DefaultSourceDir()
{
	return FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectDir(), TEXT("../content/textures/ground")));
}

bool UApexGroundTexImportCommandlet::ParseOptions(
	const FString& Params, FOptions& Out, FString& OutError)
{
	TArray<FString> Tokens;
	TArray<FString> Switches;
	TMap<FString, FString> Values;
	ParseCommandLine(*Params, Tokens, Switches, Values);

	Out.bDryRun = Switches.Contains(TEXT("dryrun"));
	Out.SourceDir = Values.Contains(TEXT("source"))
		? FPaths::ConvertRelativePathToFull(Values[TEXT("source")])
		: DefaultSourceDir();
	Out.DestRoot =
		Values.Contains(TEXT("dest")) ? Values[TEXT("dest")] : FString(ApexGround::TexturesRoot);
	if (!Out.DestRoot.StartsWith(TEXT("/Game")))
	{
		OutError = FString::Printf(
			TEXT("-dest must be a content path starting with /Game, got \"%s\""), *Out.DestRoot);
		return false;
	}
	Out.DestRoot.RemoveFromEnd(TEXT("/"));

	if (const FString* List = Values.Find(TEXT("set")))
	{
		List->ParseIntoArray(Out.Sets, TEXT(","), true);
		for (FString& Set : Out.Sets)
		{
			Set.TrimStartAndEndInline();
		}
	}
	return true;
}

bool UApexGroundTexImportCommandlet::CollectSources(
	const FOptions& Options, TArray<FSource>& OutSources, FString& OutError)
{
	if (!IFileManager::Get().DirectoryExists(*Options.SourceDir))
	{
		OutError = FString::Printf(
			TEXT("%s does not exist — run: python scripts/bake_ground_textures.py"),
			*Options.SourceDir);
		return false;
	}

	TArray<FString> Files;
	IFileManager::Get().FindFiles(
		Files, *(Options.SourceDir / TEXT("*.png")), /*Files*/ true, /*Directories*/ false);
	Files.Sort();

	for (const FString& File : Files)
	{
		// `<set>_<map>.png`, and the set may hold underscores of its own,
		// so the split is from the end.
		const FString Stem = FPaths::GetBaseFilename(File);
		FString Set;
		FString Map;
		if (!Stem.Split(TEXT("_"), &Set, &Map, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
		{
			continue;
		}
		if (!FindMapKind(Map))
		{
			UE_LOG(LogApexTrackImport, Warning,
				TEXT("  ignoring %s: %s is not one of col, nrm, rough"), *File, *Map);
			continue;
		}
		if (Options.Sets.Num() > 0 && !Options.Sets.Contains(Set))
		{
			continue;
		}
		OutSources.Add({Set, Map, Options.SourceDir / File});
	}

	if (OutSources.Num() == 0)
	{
		OutError = FString::Printf(TEXT("no ground textures to import from %s"), *Options.SourceDir);
		return false;
	}
	return true;
}

UTexture2D* UApexGroundTexImportCommandlet::ImportMap(
	const FString& PackageName, const FSource& Source, FString& OutError)
{
	const FMapKind* Kind = FindMapKind(Source.Map);
	if (!Kind)
	{
		OutError = FString::Printf(TEXT("unknown map kind %s"), *Source.Map);
		return nullptr;
	}

	FImage Image;
	if (!FImageUtils::LoadImage(*Source.Path, Image))
	{
		OutError = FString::Printf(TEXT("could not read %s"), *Source.Path);
		return nullptr;
	}
	// The gamma space is left alone on purpose. The normal and roughness
	// maps hold data, not colour, and asking FImage for a linear image would
	// have it *convert* the bytes — which turns a perfectly good normal map
	// into a flat-looking one. The bytes are kept as baked and the texture's
	// own SRGB flag says how to read them.
	Image.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		OutError = FString::Printf(TEXT("could not create package %s"), *PackageName);
		return nullptr;
	}
	Package->FullyLoad();
	const FString AssetName = FPackageName::GetShortName(PackageName);
	if (UObject* Existing = StaticFindObject(UObject::StaticClass(), Package, *AssetName))
	{
		// A fresh object every run: re-importing onto an existing texture
		// keeps its old settings, and the settings are half the point here.
		Existing->ClearFlags(RF_Public | RF_Standalone);
		Existing->Rename(nullptr, GetTransientPackage(),
			REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty);
		Existing->MarkAsGarbage();
	}

	UTexture2D* Texture = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone);
	Texture->Source.Init(Image);
	Texture->SRGB = Kind->bSRGB;
	Texture->CompressionSettings = Kind->Compression;
	Texture->LODGroup = Kind->Group;
	Texture->MipGenSettings = TMGS_FromTextureGroup;
	// The maps tile in both directions by construction; the default is wrap
	// already, but a surface that clamped would show a seam per quad and
	// nobody would guess why.
	Texture->AddressX = TA_Wrap;
	Texture->AddressY = TA_Wrap;
	Texture->UpdateResource();
	Texture->PostEditChange();
	FAssetRegistryModule::AssetCreated(Texture);
	Package->MarkPackageDirty();
	return Texture;
}

int32 UApexGroundTexImportCommandlet::Main(const FString& Params)
{
	FOptions Options;
	FString Error;
	if (!ParseOptions(Params, Options, Error))
	{
		UE_LOG(LogApexTrackImport, Error, TEXT("%s"), *Error);
		return 1;
	}

	TArray<FSource> Sources;
	if (!CollectSources(Options, Sources, Error))
	{
		UE_LOG(LogApexTrackImport, Error, TEXT("%s"), *Error);
		return 1;
	}

	UE_LOG(LogApexTrackImport, Display, TEXT("Importing %d ground texture(s) from %s into %s%s"),
		Sources.Num(), *Options.SourceDir, *Options.DestRoot,
		Options.bDryRun ? TEXT(" (dry run)") : TEXT(""));

	TSet<FString> Seen;
	TSet<UPackage*> Packages;
	int32 Failures = 0;
	for (const FSource& Source : Sources)
	{
		const FString PackageName = Options.DestRoot
			/ (FString(ApexGround::TexturePrefix) + Source.Set + TEXT("_") + Source.Map);
		UE_LOG(LogApexTrackImport, Display, TEXT("  %s <- %s"), *PackageName,
			*FPaths::GetCleanFilename(Source.Path));
		Seen.Add(Source.Set);
		if (Options.bDryRun)
		{
			continue;
		}
		if (UTexture2D* Texture = ImportMap(PackageName, Source, Error))
		{
			Packages.Add(Texture->GetOutermost());
		}
		else
		{
			UE_LOG(LogApexTrackImport, Error, TEXT("    %s"), *Error);
			++Failures;
		}
	}

	// A set the material picks but the baker did not write is worth saying
	// out loud: the surfaces it dresses silently keep the old grain, which
	// looks like the whole import having done nothing.
	for (const FString& Set : ApexGround::AllSets())
	{
		if (!Seen.Contains(Set) && Options.Sets.Num() == 0)
		{
			UE_LOG(LogApexTrackImport, Warning,
				TEXT("  the %s set is missing from %s; surfaces using it keep the procedural grain"),
				*Set, *Options.SourceDir);
		}
	}

	for (UPackage* Package : Packages)
	{
		const FString FileName = FPackageName::LongPackageNameToFilename(
			Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		if (!UPackage::SavePackage(Package, nullptr, *FileName, SaveArgs))
		{
			UE_LOG(LogApexTrackImport, Error, TEXT("    failed to save %s"), *FileName);
			++Failures;
		}
	}

	UE_LOG(LogApexTrackImport, Display, TEXT("Ground textures: %d imported, %d failure(s)"),
		Packages.Num(), Failures);
	return Failures > 0 ? 1 : 0;
}
