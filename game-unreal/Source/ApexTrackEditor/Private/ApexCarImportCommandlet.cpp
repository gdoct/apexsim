#include "ApexCarImportCommandlet.h"

#include "ApexTrackEditorModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Catalog/ApexCatalogRows.h"
#include "Catalog/ApexContentCrc.h"
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
		else if (Table == TEXT("physics") && Key == TEXT("max_steering_angle_rad"))
		{
			Out.MaxSteerRad = FCString::Atof(*Value);
		}
		else if (Table == TEXT("wheels"))
		{
			FWheelsToml& W = Out.Wheels;
			const float Number = FCString::Atof(*Value);
			if (Key == TEXT("model")) { W.Model = Value; }
			else if (Key == TEXT("front_axle_m")) { W.FrontAxleM = Number; }
			else if (Key == TEXT("rear_axle_m")) { W.RearAxleM = Number; }
			else if (Key == TEXT("front_track_m")) { W.FrontTrackM = Number; }
			else if (Key == TEXT("rear_track_m")) { W.RearTrackM = Number; }
			else if (Key == TEXT("front_radius_m")) { W.FrontRadiusM = Number; }
			else if (Key == TEXT("rear_radius_m")) { W.RearRadiusM = Number; }
			else if (Key == TEXT("front_width_m")) { W.FrontWidthM = Number; }
			else if (Key == TEXT("rear_width_m")) { W.RearWidthM = Number; }
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
	const FWheelsToml& W = Out.Wheels;
	if (W.IsPresent()
		&& (W.FrontRadiusM <= 0.0f || W.RearRadiusM <= 0.0f || W.FrontWidthM <= 0.0f || W.RearWidthM <= 0.0f
			|| W.FrontTrackM <= 0.0f || W.RearTrackM <= 0.0f || W.FrontAxleM <= W.RearAxleM))
	{
		OutError = TEXT("[wheels] needs positive radii, widths and tracks, and the front axle ahead of the rear");
		return false;
	}
	return true;
}

FString UApexCarImportCommandlet::WheelPackageName(const FString& DestRoot, const FString& Model)
{
	const FString Segment = PackageSegment(Model);
	return FString::Printf(TEXT("%s/Wheels/%s/SM_Wheel_%s"), *DestRoot, *Segment, *Segment);
}

FApexWheelSpec UApexCarImportCommandlet::MakeWheelSpec(const FCarToml& Toml, const TSoftObjectPtr<UStaticMesh>& Mesh)
{
	FApexWheelSpec Spec;
	const FWheelsToml& W = Toml.Wheels;
	if (!W.IsPresent())
	{
		return Spec;
	}
	Spec.Mesh = Mesh;
	Spec.FrontAxleM = W.FrontAxleM;
	Spec.RearAxleM = W.RearAxleM;
	Spec.FrontTrackM = W.FrontTrackM;
	Spec.RearTrackM = W.RearTrackM;
	Spec.FrontRadiusM = W.FrontRadiusM;
	Spec.RearRadiusM = W.RearRadiusM;
	Spec.FrontWidthM = W.FrontWidthM;
	Spec.RearWidthM = W.RearWidthM;
	Spec.MaxSteerRad = Toml.MaxSteerRad;
	return Spec;
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
	if (const FString* Remove = Values.Find(TEXT("remove")))
	{
		Remove->ParseIntoArray(Out.Remove, TEXT(","), true);
	}
	Out.SourceDir = Values.Contains(TEXT("source"))
		? FPaths::ConvertRelativePathToFull(Values[TEXT("source")])
		: FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../content/cars")));
	Out.WheelSourceDir = Values.Contains(TEXT("wheels"))
		? FPaths::ConvertRelativePathToFull(Values[TEXT("wheels")])
		: FPaths::ConvertRelativePathToFull(FPaths::Combine(Out.SourceDir, TEXT("../wheels")));
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
	if (!Out.bAll && !Out.bList && Out.Cars.IsEmpty() && Out.Remove.IsEmpty())
	{
		OutError = TEXT("nothing to do: pass -all, -car=FOLDER, -remove=FOLDER or -list");
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
		TArray<uint8> Bytes;
		FString Error;
		if (!FFileHelper::LoadFileToArray(Bytes, *TomlPath))
		{
			OutError = FString::Printf(TEXT("%s: unreadable"), *TomlPath);
			return false;
		}
		// Hashed from the raw bytes, exactly as the server does, before the
		// text is decoded: a BOM or an odd encoding must change both or neither.
		Source.SourceCrc = ApexContentCrc::Compute(Bytes);
		FString Text;
		FFileHelper::BufferToString(Text, Bytes.GetData(), Bytes.Num());
		if (!ParseCarToml(Text, Source.Toml, Error))
		{
			OutError = FString::Printf(TEXT("%s: %s"), *TomlPath, *Error);
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
	if (OutSources.IsEmpty() && Options.Remove.IsEmpty())
	{
		OutError = FString::Printf(TEXT("%s holds no car.toml"), *Options.SourceDir);
		return false;
	}
	return true;
}

UStaticMesh* UApexCarImportCommandlet::ImportGlb(const FString& GlbPath, const FString& PackageName, FString& OutError)
{
	const FString DestPath = FPackageName::GetLongPackagePath(PackageName);
	const FString Target = PackageName;

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
	Params.DestinationName = FPackageName::GetShortName(PackageName);

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
	UE_LOG(LogApexTrackImport, Display, TEXT("    %s: %d tris, %.0f x %.0f x %.0f cm, %d material(s)"),
		*Mesh->GetName(), Triangles, Size.X, Size.Y, Size.Z, Mesh->GetStaticMaterials().Num());
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
	UStaticMesh* Mesh = ImportGlb(GlbPath, PackageName, OutError);
	if (Mesh)
	{
		// The race car actor turns the mesh -90° about Z, so a car is expected
		// long along local Y; say so when it is not, since the turntable and
		// the cockpit layout both read the bounds.
		const FVector Size = Mesh->GetBounds().BoxExtent;
		if (Size.Y < Size.X)
		{
			UE_LOG(LogApexTrackImport, Display, TEXT("    %s is longer along X than Y: check the row's PreviewRotation"),
				*Mesh->GetName());
		}
		// Everything the import made lives beside the mesh; save the folder.
		OutPackages.Add(Mesh->GetOutermost());
		CollectDirtyPackages(Options.DestRoot / PackageSegment(Source.Folder), OutPackages);
	}
	return Mesh;
}

UStaticMesh* UApexCarImportCommandlet::ResolveWheelMesh(
	const FString& Model, const FOptions& Options, TSet<UPackage*>& OutPackages, FString& OutError)
{
	if (const TObjectPtr<UStaticMesh>* Done = WheelMeshes.Find(Model))
	{
		return *Done;
	}
	const FString PackageName = WheelPackageName(Options.DestRoot, Model);
	const FString ObjectPath = PackageName + TEXT(".") + FPackageName::GetShortName(PackageName);
	UStaticMesh* Mesh = nullptr;
	if (!Options.bForce && FPackageName::DoesPackageExist(PackageName))
	{
		Mesh = LoadObject<UStaticMesh>(nullptr, *ObjectPath);
	}
	if (!Mesh)
	{
		const FString GlbPath = Options.WheelSourceDir / Model + TEXT(".glb");
		if (!IFileManager::Get().FileExists(*GlbPath))
		{
			OutError = FString::Printf(TEXT("wheel model %s is missing"), *GlbPath);
			return nullptr;
		}
		UE_LOG(LogApexTrackImport, Display, TEXT("    importing %s <- %s"), *PackageName, *GlbPath);
		if (Options.bDryRun)
		{
			return nullptr;
		}
		Mesh = ImportGlb(GlbPath, PackageName, OutError);
		if (!Mesh)
		{
			return nullptr;
		}
		OutPackages.Add(Mesh->GetOutermost());
		CollectDirtyPackages(FPackageName::GetLongPackagePath(PackageName), OutPackages);
	}
	WheelMeshes.Add(Model, Mesh);
	return Mesh;
}

void UApexCarImportCommandlet::CollectDirtyPackages(const FString& Folder, TSet<UPackage*>& OutPackages)
{
	for (TObjectIterator<UPackage> It; It; ++It)
	{
		if (It->GetName().StartsWith(Folder + TEXT("/")) && It->IsDirty())
		{
			OutPackages.Add(*It);
		}
	}
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
		UE_LOG(LogApexTrackImport, Display, TEXT("  %s  \"%s\"  folder=%s  mesh=%s  wheels=%s%s"), *Pair.Key.ToString(),
			*Row->DisplayName, *Row->FolderName, Row->Mesh.IsNull() ? TEXT("(none)") : *Row->Mesh.ToString(),
			Row->Wheels.IsUsable() ? *Row->Wheels.Mesh.ToString() : TEXT("(none)"),
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

int32 UApexCarImportCommandlet::RemoveCars(UDataTable& Table, const TArray<FString>& Names, const FOptions& Options)
{
	int32 Failures = 0;
	TSet<UPackage*> Touched;
	for (const FString& Name : Names)
	{
		// By folder or by id; the row tells where the mesh lives.
		FName RowName;
		const FApexCarCatalogRow* Row = nullptr;
		for (const TPair<FName, uint8*>& Pair : Table.GetRowMap())
		{
			const FApexCarCatalogRow* Candidate = reinterpret_cast<const FApexCarCatalogRow*>(Pair.Value);
			if (Pair.Key.ToString().Equals(Name, ESearchCase::IgnoreCase)
				|| Candidate->FolderName.Equals(Name, ESearchCase::IgnoreCase))
			{
				RowName = Pair.Key;
				Row = Candidate;
				break;
			}
		}
		TArray<FString> Folders;
		if (Row && !Row->Mesh.IsNull())
		{
			Folders.Add(FPackageName::GetLongPackagePath(Row->Mesh.GetLongPackageName()));
		}
		Folders.AddUnique(Options.DestRoot / PackageSegment(Name));
		UE_LOG(LogApexTrackImport, Display, TEXT("  %s: %s"), *Name,
			Row ? *FString::Printf(TEXT("dropping row %s (\"%s\")"), *RowName.ToString(), *Row->DisplayName)
				: TEXT("no row"));
		if (Options.bDryRun)
		{
			continue;
		}
		if (Row)
		{
			Table.RemoveRow(RowName);
			Touched.Add(Table.GetOutermost());
		}
		for (const FString& Folder : Folders)
		{
			if (!Folder.StartsWith(Options.DestRoot + TEXT("/")))
			{
				continue;
			}
			const FString Dir = FPackageName::LongPackageNameToFilename(Folder);
			if (IFileManager::Get().DirectoryExists(*Dir))
			{
				if (IFileManager::Get().DeleteDirectory(*Dir, false, true))
				{
					UE_LOG(LogApexTrackImport, Display, TEXT("    deleted %s"), *Folder);
				}
				else
				{
					UE_LOG(LogApexTrackImport, Error, TEXT("    could not delete %s"), *Dir);
					++Failures;
				}
			}
		}
	}
	FString Error;
	if (!Options.bDryRun && Touched.Num() > 0)
	{
		Table.MarkPackageDirty();
		if (!SavePackages(Touched, Error))
		{
			UE_LOG(LogApexTrackImport, Error, TEXT("%s"), *Error);
			++Failures;
		}
	}
	return Failures;
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
	if (!Options.Remove.IsEmpty())
	{
		return RemoveCars(*Table, Options.Remove, Options) > 0 ? 1 : 0;
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

		// The wheels are derived like the checksum: follow the TOML on every
		// run. A car without a [wheels] table gets none drawn (its body has
		// its own); one whose wheel will not import is an error, since its
		// body has none.
		FApexWheelSpec Wheels;
		if (Source.Toml.Wheels.IsPresent())
		{
			FString WheelError;
			UStaticMesh* WheelMesh = ResolveWheelMesh(Source.Toml.Wheels.Model, Options, Touched, WheelError);
			if (!WheelError.IsEmpty())
			{
				UE_LOG(LogApexTrackImport, Error, TEXT("    %s"), *WheelError);
				++Failures;
				continue;
			}
			Wheels = MakeWheelSpec(Source.Toml, TSoftObjectPtr<UStaticMesh>(WheelMesh));
		}
		else
		{
			UE_LOG(LogApexTrackImport, Display, TEXT("    no [wheels] table: the body draws its own"));
		}
		const bool bNeedsWheels = Existing && Existing->Wheels != Wheels;

		// A row that already points at a mesh of its own is finished unless
		// -force: the hand-imported cars keep theirs and nothing is imported
		// twice under a second name.
		const bool bRowMeshOk = Existing && !Existing->Mesh.IsNull()
			&& (Existing->FolderName.IsEmpty() || Existing->FolderName == Source.Folder)
			&& FPackageName::DoesPackageExist(Existing->Mesh.GetLongPackageName());
		// The checksum is derived, never hand-tuned, so it follows the TOML
		// even on an additive run: it is what tells the client its import is stale.
		const bool bNeedsCrc = Existing && Existing->SourceCrc != Source.SourceCrc;
		if (bRowMeshOk && !Options.bForce)
		{
			UE_LOG(LogApexTrackImport, Display, TEXT("    keeps %s%s%s"), *Existing->Mesh.ToString(),
				bNeedsCrc ? TEXT(", updating checksum") : TEXT(""), bNeedsWheels ? TEXT(", updating wheels") : TEXT(""));
			if ((bNeedsCrc || bNeedsWheels) && !Options.bDryRun)
			{
				FApexCarCatalogRow Row = *Existing;
				Row.SourceCrc = Source.SourceCrc;
				Row.Wheels = Wheels;
				Table->AddRow(RowName, Row);
				++RowsUpdated;
				Touched.Add(Table->GetOutermost());
			}
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
		Row.SourceCrc = Source.SourceCrc;
		Row.Wheels = Wheels;
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
		if (Existing && !bFillFields && !bSetMesh && !bNeedsCrc && !bNeedsWheels)
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
