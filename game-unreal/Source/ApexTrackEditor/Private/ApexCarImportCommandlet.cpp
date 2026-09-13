#include "ApexCarImportCommandlet.h"

#include "ApexTrackEditorModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Catalog/ApexCatalogRows.h"
#include "Engine/DataTable.h"
#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "InterchangeGenericAssetsPipeline.h"
#include "InterchangeGenericAssetsPipelineSharedSettings.h"
#include "InterchangeGenericMaterialPipeline.h"
#include "InterchangeGenericMeshPipeline.h"
#include "InterchangeManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectIterator.h"

namespace
{
	const TCHAR* kGltfAssetsPipeline =
		TEXT("/Interchange/Pipelines/DefaultGLTFAssetsPipeline.DefaultGLTFAssetsPipeline");
	const TCHAR* kGltfPipeline = TEXT("/Interchange/Pipelines/DefaultGLTFPipeline.DefaultGLTFPipeline");

	/** A TOML value with its quotes and any trailing comment removed. */
	FString TomlValue(const FString& Raw)
	{
		FString Value = Raw;
		int32 Hash = INDEX_NONE;
		// A '#' outside quotes starts a comment.
		bool bInString = false;
		for (int32 i = 0; i < Value.Len(); ++i)
		{
			if (Value[i] == TEXT('"'))
			{
				bInString = !bInString;
			}
			else if (Value[i] == TEXT('#') && !bInString)
			{
				Hash = i;
				break;
			}
		}
		if (Hash != INDEX_NONE)
		{
			Value.LeftInline(Hash);
		}
		Value.TrimStartAndEndInline();
		if (Value.Len() >= 2 && Value[0] == TEXT('"') && Value[Value.Len() - 1] == TEXT('"'))
		{
			Value.MidInline(1, Value.Len() - 2);
		}
		return Value;
	}
}	 // namespace

UApexCarImportCommandlet::UApexCarImportCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
}

bool UApexCarImportCommandlet::ParseCarToml(const FString& Text, FCarToml& Out, FString& OutError)
{
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines);
	FString Table;
	for (FString Line : Lines)
	{
		Line.TrimStartAndEndInline();
		if (Line.IsEmpty() || Line.StartsWith(TEXT("#")))
		{
			continue;
		}
		if (Line.StartsWith(TEXT("[")))
		{
			// `[physics]`, `[[engine.torque_curve]]`: everything after this
			// belongs to that table until the next header.
			Table = Line.Replace(TEXT("["), TEXT("")).Replace(TEXT("]"), TEXT("")).TrimStartAndEnd();
			continue;
		}
		FString Key;
		FString Raw;
		if (!Line.Split(TEXT("="), &Key, &Raw))
		{
			continue;
		}
		Key.TrimStartAndEndInline();
		const FString Value = TomlValue(Raw);
		if (Table.IsEmpty())
		{
			if (Key == TEXT("id")) { Out.Id = Value; }
			else if (Key == TEXT("name")) { Out.Name = Value; }
			else if (Key == TEXT("model")) { Out.Model = Value; }
			else if (Key == TEXT("brand")) { Out.Brand = Value; }
			else if (Key == TEXT("class")) { Out.CarClass = Value; }
			else if (Key == TEXT("manufacturer_country")) { Out.ManufacturerCountry = Value; }
			else if (Key == TEXT("model_year")) { Out.ModelYear = FCString::Atoi(*Value); }
		}
		else if (Table == TEXT("physics") && Key == TEXT("mass_kg"))
		{
			Out.MassKg = FCString::Atof(*Value);
		}
		else if (Table == TEXT("engine") && Key == TEXT("max_power_w"))
		{
			Out.MaxPowerKw = FCString::Atof(*Value) / 1000.0f;
		}
	}
	if (Out.Id.IsEmpty() || Out.Name.IsEmpty())
	{
		OutError = TEXT("car.toml has no id or name");
		return false;
	}
	return true;
}

FString UApexCarImportCommandlet::PackageSegment(const FString& Folder)
{
	FString Segment;
	for (const TCHAR C : Folder)
	{
		Segment.AppendChar(FChar::IsAlnum(C) ? C : TEXT('_'));
	}
	return Segment;
}

FString UApexCarImportCommandlet::MeshPackageName(const FString& DestRoot, const FString& Folder)
{
	const FString Segment = PackageSegment(Folder);
	return FString::Printf(TEXT("%s/%s/SM_%s"), *DestRoot, *Segment, *Segment);
}

FString UApexCarImportCommandlet::MeshObjectPath(const FString& DestRoot, const FString& Folder)
{
	const FString Segment = PackageSegment(Folder);
	return FString::Printf(TEXT("%s/%s/SM_%s.SM_%s"), *DestRoot, *Segment, *Segment, *Segment);
}

bool UApexCarImportCommandlet::ParseOptions(const FString& Params, FOptions& Out, FString& OutError)
{
	TArray<FString> Tokens;
	TArray<FString> Switches;
	TMap<FString, FString> Values;
	ParseCommandLine(*Params, Tokens, Switches, Values);

	Out.bAll = Switches.Contains(TEXT("all"));
	Out.bList = Switches.Contains(TEXT("list"));
	Out.bForce = Switches.Contains(TEXT("force"));
	Out.bDryRun = Switches.Contains(TEXT("dryrun"));
	if (const FString* Cars = Values.Find(TEXT("car")))
	{
		Cars->ParseIntoArray(Out.Cars, TEXT(","), true);
	}
	Out.SourceDir = Values.Contains(TEXT("source"))
		? FPaths::ConvertRelativePathToFull(Values[TEXT("source")])
		: FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../content/cars")));
	Out.DestRoot = Values.Contains(TEXT("dest")) ? Values[TEXT("dest")] : TEXT("/Game/Cars");
	Out.DestRoot.RemoveFromEnd(TEXT("/"));
	Out.TablePath = Values.Contains(TEXT("table")) ? Values[TEXT("table")] : TEXT("/Game/Data/DT_CarCatalog");
	for (const FString* Path : {&Out.DestRoot, &Out.TablePath})
	{
		if (!Path->StartsWith(TEXT("/Game")))
		{
			OutError = FString::Printf(TEXT("content paths must start with /Game, got \"%s\""), **Path);
			return false;
		}
	}
	if (!Out.bAll && !Out.bList && Out.Cars.IsEmpty())
	{
		OutError = TEXT("nothing to do: pass -all, -car=FOLDER or -list");
		return false;
	}
	return true;
}

bool UApexCarImportCommandlet::CollectSources(const FOptions& Options, TArray<FSource>& OutSources, FString& OutError)
{
	if (!IFileManager::Get().DirectoryExists(*Options.SourceDir))
	{
		OutError = FString::Printf(TEXT("no cars at %s"), *Options.SourceDir);
		return false;
	}
	TArray<FString> Folders;
	IFileManager::Get().FindFiles(Folders, *(Options.SourceDir / TEXT("*")), false, true);
	Folders.Sort();
	for (const FString& Folder : Folders)
	{
		const FString TomlPath = Options.SourceDir / Folder / TEXT("car.toml");
		if (!IFileManager::Get().FileExists(*TomlPath))
		{
			continue;
		}
		if (!Options.bAll && !Options.bList && !Options.Cars.Contains(Folder))
		{
			continue;
		}
		FSource Source;
		Source.Folder = Folder;
		Source.TomlPath = TomlPath;
		FString Text;
		FString Error;
		if (!FFileHelper::LoadFileToString(Text, *TomlPath) || !ParseCarToml(Text, Source.Toml, Error))
		{
			OutError = FString::Printf(TEXT("%s: %s"), *TomlPath, Error.IsEmpty() ? TEXT("unreadable") : *Error);
			return false;
		}
		OutSources.Add(MoveTemp(Source));
	}
	for (const FString& Wanted : Options.Cars)
	{
		if (!OutSources.ContainsByPredicate([&Wanted](const FSource& S) { return S.Folder == Wanted; }))
		{
			OutError = FString::Printf(TEXT("no %s/car.toml under %s"), *Wanted, *Options.SourceDir);
			return false;
		}
	}
	if (OutSources.IsEmpty())
	{
		OutError = FString::Printf(TEXT("%s holds no car.toml"), *Options.SourceDir);
		return false;
	}
	return true;
}

UStaticMesh* UApexCarImportCommandlet::ImportGlb(
	const FString& GlbPath, const FString& DestRoot, const FString& Folder, FString& OutError)
{
	const FString Segment = PackageSegment(Folder);
	const FString DestPath = DestRoot / Segment;
	const FString Target = MeshPackageName(DestRoot, Folder);

	// The engine's glTF assets pipeline, adjusted like the prop kit's: one
	// combined mesh named for the folder, authored normals kept, no
	// collision (the server owns the physics; the client never traces a
	// car), materials as instances of the engine's glTF library next to it.
	UInterchangeGenericAssetsPipeline* Reference = LoadObject<UInterchangeGenericAssetsPipeline>(nullptr, kGltfAssetsPipeline);
	UInterchangeGenericAssetsPipeline* Pipeline = Reference
		? DuplicateObject(Reference, GetTransientPackage())
		: NewObject<UInterchangeGenericAssetsPipeline>(GetTransientPackage());
	Pipeline->bUseSourceNameForAsset = true;
	Pipeline->bSceneNameSubFolder = false;
	Pipeline->bAssetTypeSubFolders = false;
	if (Pipeline->CommonMeshesProperties)
	{
		Pipeline->CommonMeshesProperties->bRecomputeNormals = false;
		Pipeline->CommonMeshesProperties->bRecomputeTangents = true;
		Pipeline->CommonMeshesProperties->bUseMikkTSpace = true;
		Pipeline->CommonMeshesProperties->bBakeMeshes = true;
	}
	if (Pipeline->MeshPipeline)
	{
		Pipeline->MeshPipeline->bImportStaticMeshes = true;
		Pipeline->MeshPipeline->CombineStaticMeshesBehavior = EInterchangeCombineStaticMeshesBehavior::All;
		Pipeline->MeshPipeline->bBuildNanite = false;
		Pipeline->MeshPipeline->bCollision = false;
		Pipeline->MeshPipeline->bGenerateLightmapUVs = false;
	}
	if (Pipeline->MaterialPipeline)
	{
		Pipeline->MaterialPipeline->bImportMaterials = true;
		Pipeline->MaterialPipeline->bReuseExistingMaterials = false;
		Pipeline->MaterialPipeline->MaterialImport = EInterchangeMaterialImportOption::ImportAsMaterialInstances;
	}

	FImportAssetParameters Params;
	Params.bIsAutomated = true;
	Params.bReplaceExisting = true;
	Params.OverridePipelines.Add(FSoftObjectPath(Pipeline));
	Params.OverridePipelines.Add(FSoftObjectPath(kGltfPipeline));
	Params.DestinationName = TEXT("SM_") + Segment;

	// A fresh import, never a reimport (which keeps stale slots and makes
	// no materials): the old package file goes first.
	const FString File = FPackageName::LongPackageNameToFilename(Target, FPackageName::GetAssetPackageExtension());
	if (IFileManager::Get().FileExists(*File))
	{
		IFileManager::Get().Delete(*File, false, true);
		IAssetRegistry::Get()->ScanModifiedAssetFiles({File});
	}

	UInterchangeManager& Manager = UInterchangeManager::GetInterchangeManager();
	UE::Interchange::FScopedSourceData ScopedSourceData(GlbPath);
	if (!Manager.CanTranslateSourceData(ScopedSourceData.GetSourceData()))
	{
		OutError = FString::Printf(TEXT("Interchange has no translator for %s"), *GlbPath);
		return nullptr;
	}
	TArray<UObject*> Imported;
	if (!Manager.ImportAsset(DestPath, ScopedSourceData.GetSourceData(), Params, Imported))
	{
		OutError = FString::Printf(TEXT("Interchange failed on %s"), *GlbPath);
		return nullptr;
	}
	UStaticMesh* Mesh = nullptr;
	for (UObject* Object : Imported)
	{
		if (UStaticMesh* Candidate = Cast<UStaticMesh>(Object))
		{
			if (Mesh)
			{
				OutError = FString::Printf(TEXT("%s imported as more than one mesh; expected one combined mesh"), *GlbPath);
				return nullptr;
			}
			Mesh = Candidate;
		}
	}
	if (!Mesh)
	{
		OutError = FString::Printf(TEXT("%s produced no static mesh"), *GlbPath);
		return nullptr;
	}
	if (Mesh->GetOutermost()->GetName() != Target)
	{
		OutError = FString::Printf(TEXT("%s landed at %s, expected %s"), *GlbPath, *Mesh->GetPathName(), *Target);
		return nullptr;
	}
	const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
	const int32 Triangles = RenderData && !RenderData->LODResources.IsEmpty() ? Mesh->GetNumTriangles(0) : 0;
	const FVector Size = Mesh->GetBounds().BoxExtent * 2.0;
	if (Triangles <= 0 || Size.ContainsNaN() || Mesh->GetBounds().SphereRadius <= 0.0f)
	{
		OutError = FString::Printf(TEXT("%s built empty (%d triangles)"), *Mesh->GetName(), Triangles);
		return nullptr;
	}
	// The race car actor turns the mesh -90° about Z, so a car is expected
	// long along local Y; say so when it is not, since the turntable and
	// the cockpit layout both read the bounds.
	UE_LOG(LogApexTrackImport, Display, TEXT("    %s: %d tris, %.0f x %.0f x %.0f cm, %d material(s)%s"),
		*Mesh->GetName(), Triangles, Size.X, Size.Y, Size.Z, Mesh->GetStaticMaterials().Num(),
		Size.Y >= Size.X ? TEXT("") : TEXT(" — longer along X than Y: check the row's PreviewRotation"));
	return Mesh;
}

UStaticMesh* UApexCarImportCommandlet::ResolveMesh(
	const FSource& Source, const FOptions& Options, TSet<UPackage*>& OutPackages, FString& OutError)
{
	const FString PackageName = MeshPackageName(Options.DestRoot, Source.Folder);
	if (!Options.bForce && FPackageName::DoesPackageExist(PackageName))
	{
		if (UStaticMesh* Existing = LoadObject<UStaticMesh>(nullptr, *MeshObjectPath(Options.DestRoot, Source.Folder)))
		{
			return Existing;
		}
	}
	if (Source.Toml.Model.IsEmpty())
	{
		OutError = TEXT("car.toml names no model");
		return nullptr;
	}
	const FString GlbPath = FPaths::GetPath(Source.TomlPath) / Source.Toml.Model;
	if (!IFileManager::Get().FileExists(*GlbPath))
	{
		OutError = FString::Printf(TEXT("model %s is missing"), *GlbPath);
		return nullptr;
	}
	UE_LOG(LogApexTrackImport, Display, TEXT("    importing %s <- %s"), *PackageName, *GlbPath);
	if (Options.bDryRun)
	{
		return nullptr;
	}
	UStaticMesh* Mesh = ImportGlb(GlbPath, Options.DestRoot, Source.Folder, OutError);
	if (Mesh)
	{
		// Everything the import made lives beside the mesh; save the folder.
		OutPackages.Add(Mesh->GetOutermost());
		for (TObjectIterator<UPackage> It; It; ++It)
		{
			if (It->GetName().StartsWith(Options.DestRoot / PackageSegment(Source.Folder) + TEXT("/")) && It->IsDirty())
			{
				OutPackages.Add(*It);
			}
		}
	}
	return Mesh;
}

bool UApexCarImportCommandlet::SavePackages(const TSet<UPackage*>& Packages, FString& OutError)
{
	for (UPackage* Package : Packages)
	{
		if (!Package)
		{
			continue;
		}
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

void UApexCarImportCommandlet::ListRows(const UDataTable& Table, const TArray<FSource>& Sources, const FString& DestRoot)
{
	UE_LOG(LogApexTrackImport, Display, TEXT("%d row(s) in %s:"), Table.GetRowMap().Num(), *Table.GetPathName());
	for (const TPair<FName, uint8*>& Pair : Table.GetRowMap())
	{
		const FApexCarCatalogRow* Row = reinterpret_cast<const FApexCarCatalogRow*>(Pair.Value);
		const FSource* Source = Sources.FindByPredicate(
			[&Pair](const FSource& S) { return S.Toml.Id.Equals(Pair.Key.ToString(), ESearchCase::IgnoreCase); });
		UE_LOG(LogApexTrackImport, Display, TEXT("  %s  \"%s\"  folder=%s  mesh=%s%s"), *Pair.Key.ToString(),
			*Row->DisplayName, *Row->FolderName, Row->Mesh.IsNull() ? TEXT("(none)") : *Row->Mesh.ToString(),
			Source ? TEXT("") : TEXT("  [no car.toml with this id]"));
	}
	for (const FSource& Source : Sources)
	{
		if (!Table.GetRowMap().Contains(FName(*Source.Toml.Id)))
		{
			UE_LOG(LogApexTrackImport, Display, TEXT("  (no row)  %s  \"%s\"  folder=%s"), *Source.Toml.Id,
				*Source.Toml.Name, *Source.Folder);
		}
	}
}

int32 UApexCarImportCommandlet::Main(const FString& Params)
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

	const FString TableObjectPath = FString::Printf(
		TEXT("%s.%s"), *Options.TablePath, *FPackageName::GetShortName(Options.TablePath));
	UDataTable* Table = LoadObject<UDataTable>(nullptr, *TableObjectPath);
	if (!Table)
	{
		UE_LOG(LogApexTrackImport, Error, TEXT("no data table at %s"), *Options.TablePath);
		return 1;
	}
	if (Table->GetRowStruct() != FApexCarCatalogRow::StaticStruct())
	{
		UE_LOG(LogApexTrackImport, Error, TEXT("%s rows are %s, expected ApexCarCatalogRow"), *Options.TablePath,
			Table->GetRowStruct() ? *Table->GetRowStruct()->GetName() : TEXT("unset"));
		return 1;
	}
	if (Options.bList)
	{
		ListRows(*Table, Sources, Options.DestRoot);
		return 0;
	}
	if (!Options.bDryRun && !UInterchangeManager::IsInterchangeImportEnabled())
	{
		UE_LOG(LogApexTrackImport, Error, TEXT("Interchange import is disabled (Interchange.FeatureFlags.Import.Enable)"));
		return 1;
	}

	UE_LOG(LogApexTrackImport, Display, TEXT("Importing %d car(s) from %s into %s and %s%s%s"), Sources.Num(),
		*Options.SourceDir, *Options.DestRoot, *Options.TablePath, Options.bForce ? TEXT(" (force)") : TEXT(""),
		Options.bDryRun ? TEXT(" (dry run)") : TEXT(""));

	TSet<UPackage*> Touched;
	int32 RowsAdded = 0;
	int32 RowsUpdated = 0;
	int32 Failures = 0;
	for (const FSource& Source : Sources)
	{
		const FName RowName(*Source.Toml.Id);
		const FApexCarCatalogRow* Existing = Table->FindRow<FApexCarCatalogRow>(RowName, TEXT("ApexCarImport"), false);
		UE_LOG(LogApexTrackImport, Display, TEXT("  %s (\"%s\", %s)%s"), *Source.Folder, *Source.Toml.Name,
			*Source.Toml.Id, Existing ? TEXT("") : TEXT(": no row yet"));

		// A row that already points at a mesh of its own is finished unless
		// -force: the hand-imported cars keep theirs and nothing is imported
		// twice under a second name.
		const bool bRowMeshOk = Existing && !Existing->Mesh.IsNull()
			&& (Existing->FolderName.IsEmpty() || Existing->FolderName == Source.Folder)
			&& FPackageName::DoesPackageExist(Existing->Mesh.GetLongPackageName());
		if (bRowMeshOk && !Options.bForce)
		{
			UE_LOG(LogApexTrackImport, Display, TEXT("    keeps %s"), *Existing->Mesh.ToString());
			continue;
		}
		FString MeshError;
		UStaticMesh* Mesh = ResolveMesh(Source, Options, Touched, MeshError);
		if (!Mesh && !MeshError.IsEmpty())
		{
			UE_LOG(LogApexTrackImport, Error, TEXT("    %s"), *MeshError);
			++Failures;
			continue;
		}
		if (Options.bDryRun)
		{
			continue;
		}

		// A row keeps what a person tuned on it. Its mesh is replaced only
		// when it has none, or when it points at another car's folder —
		// which is how a new car ends up previewing as an old one.
		FApexCarCatalogRow Row = Existing ? *Existing : FApexCarCatalogRow();
		const bool bFillFields = !Existing || Options.bForce;
		if (bFillFields)
		{
			Row.DisplayName = Source.Toml.Name;
			Row.Brand = Source.Toml.Brand;
			Row.CarClass = Source.Toml.CarClass;
			Row.ManufacturerCountry = Source.Toml.ManufacturerCountry;
			Row.ModelYear = Source.Toml.ModelYear;
			Row.MassKg = Source.Toml.MassKg;
			Row.MaxPowerKw = Source.Toml.MaxPowerKw;
			Row.FolderName = Source.Folder;
		}
		const bool bMeshMissing = Row.Mesh.IsNull() || !FPackageName::DoesPackageExist(Row.Mesh.GetLongPackageName());
		const bool bMeshForeign = !Row.FolderName.IsEmpty() && Row.FolderName != Source.Folder;
		const bool bSetMesh = Mesh && (bFillFields || bMeshMissing || bMeshForeign);
		if (bSetMesh)
		{
			if (Existing && !bFillFields)
			{
				UE_LOG(LogApexTrackImport, Display, TEXT("    row mesh %s -> %s"),
					Row.Mesh.IsNull() ? TEXT("(none)") : *Row.Mesh.ToString(), *Mesh->GetPathName());
			}
			Row.Mesh = Mesh;
			Row.FolderName = Source.Folder;
		}
		if (Existing && !bFillFields && !bSetMesh)
		{
			continue;
		}
		Table->AddRow(RowName, Row);
		Existing ? ++RowsUpdated : ++RowsAdded;
		Touched.Add(Table->GetOutermost());
	}

	if (!Options.bDryRun && Touched.Num() > 0)
	{
		Table->MarkPackageDirty();
		if (!SavePackages(Touched, Error))
		{
			UE_LOG(LogApexTrackImport, Error, TEXT("%s"), *Error);
			return 1;
		}
	}
	UE_LOG(LogApexTrackImport, Display, TEXT("Cars: %d row(s) added, %d updated, %d package(s) saved, %d failure(s)"),
		RowsAdded, RowsUpdated, Touched.Num(), Failures);
	return Failures > 0 ? 1 : 0;
}
