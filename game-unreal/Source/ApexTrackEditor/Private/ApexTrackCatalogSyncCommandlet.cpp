#include "ApexTrackCatalogSyncCommandlet.h"

#include "ApexTrackEditorModule.h"
#include "Catalog/ApexCatalogRows.h"
#include "Dom/JsonObject.h"
#include "Engine/DataTable.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

UApexTrackCatalogSyncCommandlet::UApexTrackCatalogSyncCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
}

bool UApexTrackCatalogSyncCommandlet::ParseOptions(const FString& Params, FOptions& Out, FString& OutError)
{
	TArray<FString> Tokens;
	TArray<FString> Switches;
	TMap<FString, FString> Values;
	ParseCommandLine(*Params, Tokens, Switches, Values);

	Out.bForce = Switches.Contains(TEXT("force"));
	Out.bDryRun = Switches.Contains(TEXT("dryrun"));

	if (const FString* Manifest = Values.Find(TEXT("manifest")))
	{
		Out.ManifestPath = FPaths::ConvertRelativePathToFull(*Manifest);
	}
	else
	{
		// Same layout assumption as ApexTrackImport: the Unreal project sits
		// next to `content/`, and the manifest is gitignored build output.
		Out.ManifestPath = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectDir(), TEXT("../content/tracks/export/track_catalog.json")));
	}

	Out.TablePath = Values.Contains(TEXT("table")) ? Values[TEXT("table")] : TEXT("/Game/Data/DT_TrackCatalog");
	Out.PreviewRoot = Values.Contains(TEXT("previews")) ? Values[TEXT("previews")] : TEXT("/Game/UI/TrackPreviews");
	Out.PreviewRoot.RemoveFromEnd(TEXT("/"));

	for (const FString* Path : {&Out.TablePath, &Out.PreviewRoot})
	{
		if (!Path->StartsWith(TEXT("/Game")))
		{
			OutError = FString::Printf(TEXT("content paths must start with /Game, got \"%s\""), **Path);
			return false;
		}
	}
	return true;
}

bool UApexTrackCatalogSyncCommandlet::LoadManifest(const FString& Path, TArray<FEntry>& OutEntries, FString& OutError)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		OutError = FString::Printf(
			TEXT("no manifest at %s — bake it first with `python scripts/build_track_catalog.py`"), *Path);
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Items;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Items))
	{
		OutError = FString::Printf(TEXT("%s is not a JSON array"), *Path);
		return false;
	}

	for (const TSharedPtr<FJsonValue>& Item : Items)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!Item.IsValid() || !Item->TryGetObject(Object) || !Object)
		{
			OutError = FString::Printf(TEXT("%s: every entry must be an object"), *Path);
			return false;
		}

		FEntry Entry;
		Entry.TrackId = (*Object)->GetStringField(TEXT("track_id"));
		Entry.Stem = (*Object)->GetStringField(TEXT("stem"));
		Entry.DisplayName = (*Object)->GetStringField(TEXT("display_name"));
		Entry.Country = (*Object)->GetStringField(TEXT("country"));
		Entry.City = (*Object)->GetStringField(TEXT("city"));
		Entry.Category = (*Object)->GetStringField(TEXT("category"));
		Entry.EnvironmentType = (*Object)->GetStringField(TEXT("environment_type"));
		Entry.LengthM = static_cast<float>((*Object)->GetNumberField(TEXT("length_m")));
		Entry.PreviewPng = (*Object)->GetStringField(TEXT("preview_png"));

		if (Entry.TrackId.IsEmpty() || Entry.Stem.IsEmpty())
		{
			OutError = FString::Printf(TEXT("%s: an entry is missing track_id or stem"), *Path);
			return false;
		}
		OutEntries.Add(MoveTemp(Entry));
	}
	return true;
}

UTexture2D* UApexTrackCatalogSyncCommandlet::ImportPreview(
	const FString& PackageName, const FString& PngPath, FString& OutError)
{
	FImage Image;
	if (!FImageUtils::LoadImage(*PngPath, Image))
	{
		OutError = FString::Printf(TEXT("could not read %s"), *PngPath);
		return nullptr;
	}
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
		// Same eviction ApexTrackAssetBuilder uses: the old asset makes way
		// rather than the new one failing to take its name.
		Existing->ClearFlags(RF_Public | RF_Standalone);
		Existing->Rename(nullptr, GetTransientPackage(),
			REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty);
		Existing->MarkAsGarbage();
	}

	UTexture2D* Texture = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone);
	Texture->Source.Init(Image);
	Texture->SRGB = true;
	// UI art: shown at its own size, never minified, never streamed.
	Texture->CompressionSettings = TC_EditorIcon;
	Texture->LODGroup = TEXTUREGROUP_UI;
	Texture->MipGenSettings = TMGS_NoMipmaps;
	Texture->NeverStream = true;
	Texture->UpdateResource();
	Texture->PostEditChange();
	Package->MarkPackageDirty();
	return Texture;
}

UTexture2D* UApexTrackCatalogSyncCommandlet::ResolvePreview(
	const FEntry& Entry, const FOptions& Options, TSet<UPackage*>& TouchedPackages, FString& OutError)
{
	const FString PackageName = FString::Printf(TEXT("%s/T_Track_%s"), *Options.PreviewRoot, *Entry.Stem);
	const FString ObjectPath = FString::Printf(TEXT("%s.T_Track_%s"), *PackageName, *Entry.Stem);

	if (!Options.bForce)
	{
		if (UTexture2D* Existing = LoadObject<UTexture2D>(nullptr, *ObjectPath))
		{
			return Existing;
		}
	}

	if (Entry.PreviewPng.IsEmpty() || !IFileManager::Get().FileExists(*Entry.PreviewPng))
	{
		return nullptr;
	}

	UE_LOG(LogApexTrackImport, Display, TEXT("    importing %s from %s"), *PackageName, *Entry.PreviewPng);
	if (Options.bDryRun)
	{
		return nullptr;
	}

	UTexture2D* Texture = ImportPreview(PackageName, Entry.PreviewPng, OutError);
	if (Texture)
	{
		TouchedPackages.Add(Texture->GetOutermost());
	}
	return Texture;
}

bool UApexTrackCatalogSyncCommandlet::SavePackages(const TSet<UPackage*>& Packages, FString& OutError)
{
	for (UPackage* Package : Packages)
	{
		const FString FileName = FPackageName::LongPackageNameToFilename(
			Package->GetName(), FPackageName::GetAssetPackageExtension());

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		if (!UPackage::SavePackage(Package, nullptr, *FileName, SaveArgs))
		{
			OutError = FString::Printf(TEXT("failed to save %s"), *FileName);
			return false;
		}
	}
	return true;
}

int32 UApexTrackCatalogSyncCommandlet::Main(const FString& Params)
{
	FOptions Options;
	FString Error;
	if (!ParseOptions(Params, Options, Error))
	{
		UE_LOG(LogApexTrackImport, Error, TEXT("%s"), *Error);
		return 1;
	}

	TArray<FEntry> Entries;
	if (!LoadManifest(Options.ManifestPath, Entries, Error))
	{
		UE_LOG(LogApexTrackImport, Error, TEXT("%s"), *Error);
		return 1;
	}

	const FString TableObjectPath = FString::Printf(
		TEXT("%s.%s"), *Options.TablePath, *FPackageName::GetShortName(Options.TablePath));
	UDataTable* Table = LoadObject<UDataTable>(nullptr, *TableObjectPath);
	if (!Table)
	{
		UE_LOG(LogApexTrackImport, Error, TEXT("no data table at %s"), *Options.TablePath);
		return 1;
	}
	if (Table->GetRowStruct() != FApexTrackCatalogRow::StaticStruct())
	{
		UE_LOG(LogApexTrackImport, Error, TEXT("%s rows are %s, expected ApexTrackCatalogRow"),
			*Options.TablePath, Table->GetRowStruct() ? *Table->GetRowStruct()->GetName() : TEXT("unset"));
		return 1;
	}

	UE_LOG(LogApexTrackImport, Display, TEXT("Syncing %d track(s) from %s into %s%s%s"),
		Entries.Num(), *Options.ManifestPath, *Options.TablePath,
		Options.bForce ? TEXT(" (force)") : TEXT(""),
		Options.bDryRun ? TEXT(" (dry run)") : TEXT(""));

	TSet<UPackage*> TouchedPackages;
	int32 RowsAdded = 0;
	int32 RowsUpdated = 0;
	int32 Failures = 0;

	for (const FEntry& Entry : Entries)
	{
		const FName RowName(*Entry.TrackId);
		const FApexTrackCatalogRow* Existing = Table->FindRow<FApexTrackCatalogRow>(RowName, TEXT("ApexTrackCatalogSync"), false);

		const bool bNeedsRow = !Existing || Options.bForce;
		const bool bNeedsPreview = Existing && Existing->PreviewImage.IsNull();
		if (!bNeedsRow && !bNeedsPreview)
		{
			continue;
		}

		UE_LOG(LogApexTrackImport, Display, TEXT("%s (%s): %s"), *Entry.Stem, *Entry.TrackId,
			bNeedsRow ? (Existing ? TEXT("rewriting row") : TEXT("adding row")) : TEXT("filling in preview"));

		FString PreviewError;
		UTexture2D* Preview = ResolvePreview(Entry, Options, TouchedPackages, PreviewError);
		if (!Preview && !PreviewError.IsEmpty())
		{
			UE_LOG(LogApexTrackImport, Error, TEXT("    %s"), *PreviewError);
			++Failures;
			continue;
		}
		if (!Preview && !Options.bDryRun)
		{
			UE_LOG(LogApexTrackImport, Warning,
				TEXT("    no preview PNG for %s; the card will show the placeholder"), *Entry.Stem);
		}

		if (Options.bDryRun)
		{
			continue;
		}

		FApexTrackCatalogRow Row = bNeedsRow ? FApexTrackCatalogRow() : *Existing;
		if (bNeedsRow)
		{
			Row.DisplayName = Entry.DisplayName;
			Row.Country = Entry.Country;
			Row.City = Entry.City;
			Row.Category = Entry.Category;
			Row.EnvironmentType = Entry.EnvironmentType;
			Row.LengthM = Entry.LengthM;
			Row.YamlBaseName = Entry.Stem;
		}
		if (Preview)
		{
			Row.PreviewImage = Preview;
		}

		// AddRow replaces an existing row of the same name.
		Table->AddRow(RowName, Row);
		if (Existing)
		{
			++RowsUpdated;
		}
		else
		{
			++RowsAdded;
		}
		TouchedPackages.Add(Table->GetOutermost());
	}

	if (!Options.bDryRun && TouchedPackages.Num() > 0)
	{
		Table->MarkPackageDirty();
		if (!SavePackages(TouchedPackages, Error))
		{
			UE_LOG(LogApexTrackImport, Error, TEXT("%s"), *Error);
			return 1;
		}
	}

	UE_LOG(LogApexTrackImport, Display, TEXT("Catalog sync: %d row(s) added, %d updated, %d package(s) saved, %d failure(s)"),
		RowsAdded, RowsUpdated, TouchedPackages.Num(), Failures);
	return Failures > 0 ? 1 : 0;
}
