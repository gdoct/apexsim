#include "Cars/ApexCarContentSubsystem.h"

#include "ApexSim.h"
#include "Async/Async.h"
#include "Cars/ApexCarMaterials.h"
#include "Cars/ApexCarToml.h"
#include "Cars/ApexGlbReader.h"
#include "Catalog/ApexContentCrc.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/DataTable.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "IImageWrapperModule.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"
#include "TextureResource.h"

namespace
{
	FAutoConsoleCommand CarRescanCommand(TEXT("apexsim.car.Rescan"),
		TEXT("Read the car folders again and drop every car built from them; the next car drawn is rebuilt from its files."),
		FConsoleCommandDelegate::CreateLambda([]() {
			if (UApexCarContentSubsystem* Content = UApexCarContentSubsystem::Get())
			{
				Content->Rescan();
			}
		}));

	const TCHAR* kCarTablePath = TEXT("/Game/Data/DT_CarCatalog.DT_CarCatalog");
	/** What a car draws with when the car parents were never baked: flat colours, not nothing. */
	const TCHAR* kCarFallbackMaterial = TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial");

	/** `<model>.glb` in the wheels folder beside a cars folder, or empty. */
	FString FindWheel(const FString& CarsDir, const FString& Model)
	{
		// `wheels` in the repo, `Wheels` beside the packaged game; the same
		// folder on Windows, two on anything else.
		for (const TCHAR* Sub : {TEXT("wheels"), TEXT("Wheels")})
		{
			const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(CarsDir, TEXT(".."), Sub, Model + TEXT(".glb")));
			if (IFileManager::Get().FileExists(*Path))
			{
				return Path;
			}
		}
		return FString();
	}

	/** A file named relative to a car folder, as a full path; empty (and a warning) when it is not there. */
	FString CarFile(const FString& CarDir, const FString& Relative, const FString& Folder, const TCHAR* What)
	{
		if (Relative.IsEmpty())
		{
			return FString();
		}
		const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(CarDir, Relative));
		if (!IFileManager::Get().FileExists(*Path))
		{
			UE_LOG(LogApexSim, Warning, TEXT("Runtime cars: %s names %s %s, which is not there"), *Folder, What, *Path);
			return FString();
		}
		return Path;
	}

	/**
	 * The wheel a `[wheels] model` / `rear_model` names, as a full path: a GLB
	 * in the car's own folder (ApexCarToml::IsCarLocalWheel), else the class
	 * wheel in the wheels folder beside the cars folder it was found in, then
	 * beside any other. Empty, and a warning, when it is nowhere.
	 */
	FString ResolveWheel(const FString& CarsDir, const FString& CarDir, const FString& Model, const FString& Folder)
	{
		if (ApexCarToml::IsCarLocalWheel(Model))
		{
			return CarFile(CarDir, Model, Folder, TEXT("its wheel"));
		}
		FString Path = FindWheel(CarsDir, Model);
		const TArray<FString> Dirs = UApexCarContentSubsystem::CarDirectories();
		for (int32 i = 0; Path.IsEmpty() && i < Dirs.Num(); ++i)
		{
			Path = FindWheel(Dirs[i], Model);
		}
		if (Path.IsEmpty())
		{
			UE_LOG(LogApexSim, Warning, TEXT("Runtime cars: %s wants the %s wheel, and no wheels folder beside the cars has %s.glb"),
				*Folder, *Model, *Model);
		}
		return Path;
	}

	/**
	 * Per-vertex tangents from the UV gradient, orthogonalised against the
	 * normals, with the binormal's handedness as a sign; the fast build
	 * takes them as they are. (No car material samples a normal map yet;
	 * the clear coat and the translucent glass still shade with them.)
	 */
	void ComputeCarTangents(const FApexGlbModel& Model, TArray<FVector3f>& OutTangents, TArray<float>& OutSigns)
	{
		const int32 Count = Model.Positions.Num();
		TArray<FVector3f> Tangents;
		TArray<FVector3f> Bitangents;
		Tangents.Init(FVector3f::ZeroVector, Count);
		Bitangents.Init(FVector3f::ZeroVector, Count);
		for (const FApexGlbSection& Section : Model.Sections)
		{
			for (int32 Tri = 0; Tri + 2 < Section.Indices.Num(); Tri += 3)
			{
				const uint32 A = Section.Indices[Tri];
				const uint32 B = Section.Indices[Tri + 1];
				const uint32 C = Section.Indices[Tri + 2];
				const FVector3f E1 = Model.Positions[B] - Model.Positions[A];
				const FVector3f E2 = Model.Positions[C] - Model.Positions[A];
				const FVector2f D1 = Model.UVs[B] - Model.UVs[A];
				const FVector2f D2 = Model.UVs[C] - Model.UVs[A];
				const float Det = D1.X * D2.Y - D2.X * D1.Y;
				if (FMath::Abs(Det) < 1.0e-12f)
				{
					continue;
				}
				const float R = 1.0f / Det;
				const FVector3f T = (E1 * D2.Y - E2 * D1.Y) * R;
				const FVector3f Bt = (E2 * D1.X - E1 * D2.X) * R;
				for (const uint32 V : {A, B, C})
				{
					Tangents[V] += T;
					Bitangents[V] += Bt;
				}
			}
		}
		OutTangents.SetNumUninitialized(Count);
		OutSigns.SetNumUninitialized(Count);
		for (int32 i = 0; i < Count; ++i)
		{
			const FVector3f& N = Model.Normals[i];
			FVector3f T = Tangents[i] - N * FVector3f::DotProduct(N, Tangents[i]);
			if (!T.Normalize())
			{
				const FVector3f Axis = FMath::Abs(N.Z) < 0.9f ? FVector3f(0.0f, 0.0f, 1.0f) : FVector3f(1.0f, 0.0f, 0.0f);
				T = FVector3f::CrossProduct(Axis, N).GetSafeNormal();
			}
			OutTangents[i] = T;
			OutSigns[i] = FVector3f::DotProduct(FVector3f::CrossProduct(N, T), Bitangents[i]) < 0.0f ? -1.0f : 1.0f;
		}
	}
}	 // namespace

UApexCarContentSubsystem* UApexCarContentSubsystem::Get()
{
	return GEngine ? GEngine->GetEngineSubsystem<UApexCarContentSubsystem>() : nullptr;
}

void UApexCarContentSubsystem::Deinitialize()
{
	// The reads own everything they touch, but must not outlive the module.
	for (TPair<FString, TFuture<TSharedPtr<FParsedModel>>>& InFlight : Pending)
	{
		InFlight.Value.Wait();
	}
	Pending.Reset();
	DropBuilt();
	Super::Deinitialize();
}

TArray<FString> UApexCarContentSubsystem::CarDirectories()
{
	TArray<FString> Dirs;
	FString Override;
	if (FParse::Value(FCommandLine::Get(), TEXT("-ApexCarsDir="), Override))
	{
		TArray<FString> Parts;
		Override.ParseIntoArray(Parts, TEXT("+"), true);
		for (const FString& Part : Parts)
		{
			Dirs.Add(FPaths::ConvertRelativePathToFull(Part));
		}
	}
	// In a packaged build ProjectDir is <Release>/Game/ApexSim/, so its parent
	// holds ApexSim.exe, `Tracks/` and `Cars/`. In the editor the cars never
	// leave the repo.
	const FString Default = FPlatformProperties::RequiresCookedData()
		? FPaths::Combine(FPaths::ProjectDir(), TEXT(".."), TEXT("Cars"))
		: FPaths::Combine(FPaths::ProjectDir(), TEXT(".."), TEXT("content"), TEXT("cars"));
	Dirs.AddUnique(FPaths::ConvertRelativePathToFull(Default));
	return Dirs;
}

TArray<FString> UApexCarContentSubsystem::CarFolders(const FString& CarsDir)
{
	// The shipped cars, then the player's own: the first to hold an id wins,
	// so an import can never replace a shipped car by reusing its id.
	TArray<FString> Folders;
	for (const TCHAR* Sub : {TEXT("default"), TEXT("custom")})
	{
		const FString Folder = FPaths::Combine(CarsDir, Sub);
		if (IFileManager::Get().DirectoryExists(*Folder))
		{
			Folders.Add(Folder);
		}
	}
	// A folder with neither (an -ApexCarsDir of loose cars) holds the cars itself.
	if (Folders.IsEmpty())
	{
		Folders.Add(CarsDir);
	}
	return Folders;
}

void UApexCarContentSubsystem::EnsureScanned() const
{
	if (!bScanned)
	{
		// Read on first use rather than at engine start: commandlets and the
		// cooker create engine subsystems too and have no use for the cars.
		const_cast<UApexCarContentSubsystem*>(this)->ScanNow();
	}
}

void UApexCarContentSubsystem::Rescan()
{
	for (TPair<FString, TFuture<TSharedPtr<FParsedModel>>>& InFlight : Pending)
	{
		InFlight.Value.Wait();
	}
	Pending.Reset();
	DropBuilt();
	ScanNow();
}

void UApexCarContentSubsystem::DropBuilt()
{
	// Cars already on screen keep what they were given until they are dressed again.
	Models.Reset();
	Images.Reset();
	EmbeddedImages.Reset();
	Broken.Reset();
}

void UApexCarContentSubsystem::ScanNow()
{
	bScanned = true;
	RuntimeRows.Reset();
	TableRows.Reset();

	// The table: the fallback for a car with no folder here, and the hand-tuned
	// framing and cockpit points a runtime car borrows. Tested first: a fresh
	// clone may have none, which is no reason for an error.
	const FString TablePackage = FPackageName::ObjectPathToPackageName(FString(kCarTablePath));
	if (const UDataTable* Table = FPackageName::DoesPackageExist(TablePackage) ? LoadObject<UDataTable>(nullptr, kCarTablePath) : nullptr)
	{
		if (Table->GetRowStruct() == FApexCarCatalogRow::StaticStruct())
		{
			for (const TPair<FName, uint8*>& Pair : Table->GetRowMap())
			{
				TableRows.Add(Pair.Key.ToString().ToLower(), *reinterpret_cast<const FApexCarCatalogRow*>(Pair.Value));
			}
		}
	}

	int32 Skipped = 0;
	TArray<FString> Found;
	TMap<FString, FString> SourceOf; // id -> the car folder it was read from
	for (const FString& Dir : CarDirectories())
	{
		if (!IFileManager::Get().DirectoryExists(*Dir))
		{
			UE_LOG(LogApexSim, Log, TEXT("Runtime cars: no folder at %s"), *Dir);
			continue;
		}
		for (const FString& Parent : CarFolders(Dir))
		{
			TArray<FString> Folders;
			IFileManager::Get().FindFiles(Folders, *FPaths::Combine(Parent, TEXT("*")), false, true);
			Folders.Sort();
			for (const FString& Folder : Folders)
			{
				const FString CarDir = FPaths::Combine(Parent, Folder);
				const FString TomlPath = FPaths::Combine(CarDir, TEXT("car.toml"));
				TArray<uint8> Bytes;
				if (!IFileManager::Get().FileExists(*TomlPath))
				{
					continue;
				}
				if (!FFileHelper::LoadFileToArray(Bytes, *TomlPath))
				{
					UE_LOG(LogApexSim, Warning, TEXT("Runtime cars: %s is unreadable; skipped"), *TomlPath);
					++Skipped;
					continue;
				}
				FString Text;
				FFileHelper::BufferToString(Text, Bytes.GetData(), Bytes.Num());
				FApexCarToml Toml;
				FString Error;
				if (!ApexCarToml::Parse(Text, Toml, Error))
				{
					UE_LOG(LogApexSim, Warning, TEXT("Runtime cars: %s: %s; skipped"), *TomlPath, *Error);
					++Skipped;
					continue;
				}
				const FString IdKey = Toml.Id.ToLower();
				if (const FString* Earlier = SourceOf.Find(IdKey))
				{
					// An earlier folder wins: that is what -ApexCarsDir is for, and
					// what keeps a custom car from replacing a shipped one. The same
					// folder name under a later root is a copy of it, not a clash.
					if (!FPaths::GetCleanFilename(*Earlier).Equals(Folder))
					{
						UE_LOG(LogApexSim, Warning, TEXT("Runtime cars: %s has the id of %s (%s); the first one found is used"),
							*CarDir, **Earlier, *Toml.Id);
					}
					continue;
				}
				SourceOf.Add(IdKey, CarDir);

				FApexCarCatalogRow Row;
				Row.DisplayName = Toml.Name;
				Row.Brand = Toml.Brand;
				Row.CarClass = Toml.CarClass;
				Row.ManufacturerCountry = Toml.ManufacturerCountry;
				Row.ModelYear = Toml.ModelYear;
				Row.MassKg = Toml.MassKg;
				Row.MaxPowerKw = Toml.MaxPowerKw;
				Row.FolderName = Folder;
				// Hashed from the raw bytes, exactly as the server does.
				Row.SourceCrc = ApexContentCrc::Compute(Bytes);
				Row.RuntimeModel = CarFile(CarDir, Toml.Model, Folder, TEXT("its model"));
				Row.EngineSound = Toml.Sound;
				Row.TyreOptimalC = Toml.TyreOptimalC;
				Row.TyreWindowC = Toml.TyreWindowC;

				Row.Wheels = ApexCarToml::MakeWheelSpec(Toml);
				if (Toml.Wheels.IsPresent())
				{
					Row.Wheels.RuntimeModel = ResolveWheel(Dir, CarDir, Toml.Wheels.Model, Folder);
					if (!Toml.Wheels.RearModel.IsEmpty())
					{
						// Missing, the rears fall back to the front's model rather than vanish.
						Row.Wheels.RearRuntimeModel = ResolveWheel(Dir, CarDir, Toml.Wheels.RearModel, Folder);
					}
				}
				Row.DrsFlap = ApexCarToml::MakeDrsFlapSpec(Toml);
				if (Toml.DrsFlap.IsPresent())
				{
					Row.DrsFlap.RuntimeModel = CarFile(CarDir, Toml.DrsFlap.Model, Folder, TEXT("its DRS flap"));
				}
				if (Toml.Driver.IsPresent())
				{
					Row.Driver.RuntimeModel = CarFile(CarDir, Toml.Driver.Model, Folder, TEXT("its driver"));
				}
				Row.DamageParts = ApexCarToml::MakeDamageParts(Toml);
				for (const FApexCarLiveryToml& Source : Toml.Liveries)
				{
					FApexCarLivery& Livery = Row.Liveries.AddDefaulted_GetRef();
					Livery.Name = Source.Name;
					Livery.Paint = Source.Paint;
					Livery.Accent = Source.Accent;
					Livery.PaintMetallic = Source.Metallic;
					Livery.RuntimeLogo = CarFile(CarDir, Source.Logo, Folder, TEXT("a livery logo"));
					// A skin or texture that is not there leaves its slots as authored.
					Livery.RuntimeSkin = CarFile(CarDir, Source.Skin, Folder, TEXT("a livery skin"));
					for (const TPair<FString, FString>& Texture : Source.Textures)
					{
						const FString Path = CarFile(CarDir, Texture.Value, Folder, TEXT("a livery texture"));
						if (!Path.IsEmpty())
						{
							FApexLiveryTexture& Entry = Livery.RuntimeTextures.AddDefaulted_GetRef();
							Entry.Slot = FName(*Texture.Key);
							Entry.RuntimeTexture = Path;
						}
					}
					Livery.RuntimePreview = CarFile(CarDir, Source.Preview, Folder, TEXT("a livery preview"));
				}

				const FApexCarCatalogRow* TableRow = TableRows.Find(IdKey);
				if (Toml.Preview.bPresent)
				{
					Row.PreviewOffset = Toml.Preview.OffsetCm;
					Row.PreviewRotation = Toml.Preview.Rotation;
					Row.PreviewScale = Toml.Preview.Scale;
				}
				else if (TableRow)
				{
					Row.PreviewOffset = TableRow->PreviewOffset;
					Row.PreviewRotation = TableRow->PreviewRotation;
					Row.PreviewScale = TableRow->PreviewScale;
				}
				if (Toml.bHasCockpit)
				{
					// Key by key over the table's hand-tuned points: a car.toml
					// that only names its wheel lock keeps the framing.
					Row.Cockpit = ApexCarContent::MergeCockpit(TableRow ? &TableRow->Cockpit : nullptr, Toml.Cockpit);
					Row.Cockpit.RuntimeSteeringWheel = CarFile(CarDir, Toml.SteeringWheelModel, Folder, TEXT("its steering wheel"));
				}
				else if (TableRow)
				{
					Row.Cockpit = TableRow->Cockpit;
				}

				Found.Add(Folder);
				RuntimeRows.Add(IdKey, MoveTemp(Row));
			}
		}
	}

	int32 FromTableOnly = 0;
	for (const TPair<FString, FApexCarCatalogRow>& Pair : TableRows)
	{
		FromTableOnly += RuntimeRows.Contains(Pair.Key) ? 0 : 1;
	}
	UE_LOG(LogApexSim, Log, TEXT("Runtime cars: %d found (%s)%s%s"), Found.Num(), *FString::Join(Found, TEXT(", ")),
		Skipped > 0 ? *FString::Printf(TEXT(", %d skipped"), Skipped) : TEXT(""),
		FromTableOnly > 0 ? *FString::Printf(TEXT("; %d more only in DT_CarCatalog"), FromTableOnly) : TEXT(""));
}

const FApexCarCatalogRow* UApexCarContentSubsystem::FindRow(const FString& CarId) const
{
	if (CarId.IsEmpty())
	{
		return nullptr;
	}
	EnsureScanned();
	const FString Key = CarId.ToLower();
	if (const FApexCarCatalogRow* Runtime = RuntimeRows.Find(Key))
	{
		return Runtime;
	}
	return TableRows.Find(Key);
}

void UApexCarContentSubsystem::ForEachRow(TFunctionRef<void(const FString& CarId, const FApexCarCatalogRow& Row)> Fn) const
{
	EnsureScanned();
	for (const TPair<FString, FApexCarCatalogRow>& Pair : RuntimeRows)
	{
		Fn(Pair.Key, Pair.Value);
	}
	for (const TPair<FString, FApexCarCatalogRow>& Pair : TableRows)
	{
		if (!RuntimeRows.Contains(Pair.Key))
		{
			Fn(Pair.Key, Pair.Value);
		}
	}
}

int32 UApexCarContentSubsystem::NumRuntimeCars() const
{
	EnsureScanned();
	return RuntimeRows.Num();
}

FString UApexCarContentSubsystem::ModelKey(const FString& Path)
{
	FString Full = FPaths::ConvertRelativePathToFull(Path);
	FPaths::NormalizeFilename(Full);
	return Full.ToLower();
}

IImageWrapperModule* UApexCarContentSubsystem::ImageWrappers()
{
	// Loaded here, on the game thread, so the reads can use it anywhere.
	return &FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName(TEXT("ImageWrapper")));
}

TSharedPtr<UApexCarContentSubsystem::FParsedModel> UApexCarContentSubsystem::ParseModel(
	const FString& Path, IImageWrapperModule* Wrappers)
{
	const double Began = FPlatformTime::Seconds();
	TSharedPtr<FParsedModel> Result = MakeShared<FParsedModel>();
	Result->Model = MakeShared<FApexGlbModel>();
	if (!ApexGlb::ReadFile(Path, *Result->Model, Result->Error))
	{
		return Result;
	}
	if (Wrappers)
	{
		FString ImageError;
		if (!ApexGlb::DecodeImages(*Result->Model, *Wrappers, ImageError))
		{
			// A picture that will not decode leaves its slot untextured, not the car undrawn.
			Result->Warning = ImageError;
		}
	}
	Result->Seconds = FPlatformTime::Seconds() - Began;
	Result->bOk = true;
	return Result;
}

void UApexCarContentSubsystem::Prefetch(const FApexCarCatalogRow& Row)
{
	IImageWrapperModule* Wrappers = nullptr;
	for (const FString* Path : {&Row.RuntimeModel, &Row.Wheels.RuntimeModel, &Row.Wheels.RearRuntimeModel, &Row.DrsFlap.RuntimeModel, &Row.Driver.RuntimeModel,
			 &Row.Cockpit.RuntimeSteeringWheel})
	{
		if (Path->IsEmpty())
		{
			continue;
		}
		const FString Key = ModelKey(*Path);
		if (Models.Contains(Key) || Pending.Contains(Key) || Broken.Contains(Key))
		{
			continue;
		}
		if (!Wrappers)
		{
			Wrappers = ImageWrappers();
		}
		const FString File = *Path;
		Pending.Add(Key, Async(EAsyncExecution::ThreadPool, [File, Wrappers]() { return ParseModel(File, Wrappers); }));
	}
}

TSharedPtr<UApexCarContentSubsystem::FParsedModel> UApexCarContentSubsystem::TakeParsed(const FString& Path)
{
	const FString Key = ModelKey(Path);
	TSharedPtr<FParsedModel> Parsed;
	if (TFuture<TSharedPtr<FParsedModel>>* InFlight = Pending.Find(Key))
	{
		Parsed = InFlight->Get();
		Pending.Remove(Key);
	}
	else
	{
		Parsed = ParseModel(Path, ImageWrappers());
	}
	if (Parsed && Parsed->bOk)
	{
		if (!Parsed->Warning.IsEmpty())
		{
			UE_LOG(LogApexSim, Warning, TEXT("Car model %s: %s"), *Path, *Parsed->Warning);
		}
		return Parsed;
	}
	UE_LOG(LogApexSim, Error, TEXT("Car model %s could not be read: %s"), *Path,
		Parsed ? *Parsed->Error : TEXT("the reader returned nothing"));
	return nullptr;
}

UStaticMesh* UApexCarContentSubsystem::LoadModel(const FString& Path)
{
	if (Path.IsEmpty())
	{
		return nullptr;
	}
	const FString Key = ModelKey(Path);
	if (const TObjectPtr<UStaticMesh>* Built = Models.Find(Key))
	{
		return *Built;
	}
	if (Broken.Contains(Key))
	{
		return nullptr;
	}
	const TSharedPtr<FParsedModel> Parsed = TakeParsed(Path);
	UStaticMesh* Mesh = Parsed ? BuildModel(Path, *Parsed->Model, ModelTextures(Key, *Parsed->Model)) : nullptr;
	if (!Mesh)
	{
		Broken.Add(Key);
		return nullptr;
	}
	UE_LOG(LogApexSim, Log, TEXT("Car model %s: read in %.0f ms"), *FPaths::GetCleanFilename(Path), Parsed->Seconds * 1000.0);
	Models.Add(Key, Mesh);
	return Mesh;
}

TArray<UStaticMesh*> UApexCarContentSubsystem::LoadModelPieces(const FString& Path, const TArray<FApexDamagePartSpec>& Parts)
{
	TArray<UStaticMesh*> Out;
	if (Path.IsEmpty())
	{
		return Out;
	}
	const FString Key = ModelKey(Path);
	// One set of pieces per GLB and set of boxes: two cars sharing a GLB
	// with different parts are two sets.
	FString Boxes;
	for (const FApexDamagePartSpec& Part : Parts)
	{
		Boxes += FString::Printf(TEXT("%s:%s:%s;"), *Part.Name, *Part.MinCm.ToString(), *Part.MaxCm.ToString());
	}
	const FString PieceKey = FString::Printf(TEXT("%s#pieces%08x"), *Key, GetTypeHash(Boxes));
	auto PieceName = [&PieceKey](int32 Index) { return FString::Printf(TEXT("%s#%d"), *PieceKey, Index); };
	auto Collect = [this, &PieceName, &Parts, &Out]() {
		for (int32 i = 0; i <= Parts.Num(); ++i)
		{
			const TObjectPtr<UStaticMesh>* Built = Models.Find(PieceName(i));
			Out.Add(Built ? Built->Get() : nullptr);
		}
	};
	if (Models.Contains(PieceName(0)))
	{
		Collect();
		return Out;
	}
	if (Broken.Contains(PieceKey))
	{
		return Out;
	}
	const double Began = FPlatformTime::Seconds();
	const TSharedPtr<FParsedModel> Parsed = TakeParsed(Path);
	if (!Parsed)
	{
		Broken.Add(PieceKey);
		return Out;
	}
	const FApexGlbModel& Model = *Parsed->Model;
	TArray<FBox3f> PartBoxes;
	for (const FApexDamagePartSpec& Part : Parts)
	{
		PartBoxes.Add(FBox3f(FVector3f(Part.MinCm), FVector3f(Part.MaxCm)));
	}
	TArray<FApexGlbModel> Pieces;
	ApexGlb::SplitByBoxes(Model, PartBoxes, Pieces);
	const TArray<UTexture2D*> Textures = ModelTextures(Key, Model);
	for (int32 i = 0; i < Pieces.Num(); ++i)
	{
		if (Pieces[i].Sections.IsEmpty())
		{
			if (i > 0)
			{
				UE_LOG(LogApexSim, Warning, TEXT("Car model %s: the damage part %s holds no triangle; it never comes off"),
					*FPaths::GetCleanFilename(Path), *Parts[i - 1].Name);
			}
			continue;
		}
		// The body keeps the whole car's bounds: the cockpit, the
		// headlights and the damage's own frame are all read off them.
		UStaticMesh* Mesh = BuildModel(Path, Pieces[i], Textures,
			i == 0 ? FString(TEXT("_body")) : TEXT("_") + ApexCarToml::Segment(Parts[i - 1].Name),
			i == 0 ? Model.Bounds : FBox3f(ForceInit));
		if (Mesh)
		{
			Models.Add(PieceName(i), Mesh);
		}
	}
	if (!Models.Contains(PieceName(0)))
	{
		Broken.Add(PieceKey);
		return Out;
	}
	UE_LOG(LogApexSim, Log, TEXT("Car model %s: split into the body and %d damage part(s) in %.0f ms"),
		*FPaths::GetCleanFilename(Path), Parts.Num(), (FPlatformTime::Seconds() - Began) * 1000.0);
	Collect();
	return Out;
}

TArray<UTexture2D*> UApexCarContentSubsystem::ModelTextures(const FString& Key, const FApexGlbModel& Model)
{
	const FString Stem = ApexCarToml::Segment(FPaths::GetBaseFilename(Key));
	TArray<UTexture2D*> Textures;
	Textures.SetNumZeroed(Model.Images.Num());
	for (int32 i = 0; i < Model.Images.Num(); ++i)
	{
		if (!Model.Images[i].bUsed)
		{
			continue;
		}
		const FString ImageKey = FString::Printf(TEXT("%s#%d"), *Key, i);
		if (const TObjectPtr<UTexture2D>* Made = EmbeddedImages.Find(ImageKey))
		{
			Textures[i] = *Made;
			continue;
		}
		Textures[i] = MakeTexture(Model.Images[i], FString::Printf(TEXT("%s_%d"), *Stem, i));
		if (Textures[i])
		{
			EmbeddedImages.Add(ImageKey, Textures[i]);
		}
	}
	return Textures;
}

UTexture2D* UApexCarContentSubsystem::MakeTexture(const FApexGlbImage& Image, const FString& Name)
{
	if (Image.Mips.Num() == 0 || Image.Width <= 0 || Image.Height <= 0)
	{
		return nullptr;
	}
	const FName ObjectName = MakeUniqueObjectName(GetTransientPackage(), UTexture2D::StaticClass(), FName(*(TEXT("T_") + Name)));
	UTexture2D* Texture = UTexture2D::CreateTransient(Image.Width, Image.Height, PF_B8G8R8A8, ObjectName);
	FTexturePlatformData* Platform = Texture ? Texture->GetPlatformData() : nullptr;
	if (!Platform || Platform->Mips.Num() != 1)
	{
		return nullptr;
	}
	// CreateTransient sizes the top level; the rest of the chain is added
	// here, so a logo seen from down the grid does not shimmer.
	for (int32 Level = 0; Level < Image.Mips.Num(); ++Level)
	{
		const TArray<uint8>& Pixels = Image.Mips[Level];
		FTexture2DMipMap* Mip = nullptr;
		if (Level == 0)
		{
			Mip = &Platform->Mips[0];
		}
		else
		{
			Mip = new FTexture2DMipMap(FMath::Max(1, Image.Width >> Level), FMath::Max(1, Image.Height >> Level), 1);
			Platform->Mips.Add(Mip);
		}
		void* Data = Mip->BulkData.Lock(LOCK_READ_WRITE);
		if (Level > 0 || Mip->BulkData.GetBulkDataSize() != Pixels.Num())
		{
			Data = Mip->BulkData.Realloc(Pixels.Num());
		}
		FMemory::Memcpy(Data, Pixels.GetData(), Pixels.Num());
		Mip->BulkData.Unlock();
	}
	Texture->SRGB = true;
	Texture->AddressX = TA_Wrap;
	Texture->AddressY = TA_Wrap;
	Texture->UpdateResource();
	return Texture;
}

UTexture2D* UApexCarContentSubsystem::LoadTexture(const FString& Path)
{
	if (Path.IsEmpty())
	{
		return nullptr;
	}
	const FString Key = ModelKey(Path);
	if (const TObjectPtr<UTexture2D>* Loaded = Images.Find(Key))
	{
		return *Loaded;
	}
	if (Broken.Contains(Key))
	{
		return nullptr;
	}
	FApexGlbImage Image;
	UTexture2D* Texture = nullptr;
	if (FFileHelper::LoadFileToArray(Image.Encoded, *Path) && ApexGlb::DecodeImage(Image, *ImageWrappers()))
	{
		Texture = MakeTexture(Image, ApexCarToml::Segment(FPaths::GetBaseFilename(Path)));
	}
	if (!Texture)
	{
		UE_LOG(LogApexSim, Warning, TEXT("Car image %s could not be loaded"), *Path);
		Broken.Add(Key);
		return nullptr;
	}
	Images.Add(Key, Texture);
	return Texture;
}

UStaticMesh* UApexCarContentSubsystem::BuildModel(const FString& Path, const FApexGlbModel& Model,
	const TArray<UTexture2D*>& Textures, const FString& Suffix, const FBox3f& Bounds)
{
	const double Began = FPlatformTime::Seconds();
	const FString Stem = ApexCarToml::Segment(FPaths::GetBaseFilename(Path)) + Suffix;

	if (!bParentsLoaded)
	{
		bParentsLoaded = true;
		const FApexCarParents Loaded = ApexCarMaterials::LoadParents();
		Parents = {Loaded.Opaque, Loaded.ClearCoat, Loaded.Masked, Loaded.Translucent};
		FallbackMaterial = LoadObject<UMaterialInterface>(nullptr, kCarFallbackMaterial);
	}
	FApexCarParents CarParents;
	CarParents.Opaque = Parents.IsValidIndex(0) ? Parents[0].Get() : nullptr;
	CarParents.ClearCoat = Parents.IsValidIndex(1) ? Parents[1].Get() : nullptr;
	CarParents.Masked = Parents.IsValidIndex(2) ? Parents[2].Get() : nullptr;
	CarParents.Translucent = Parents.IsValidIndex(3) ? Parents[3].Get() : nullptr;
	if (!CarParents.IsComplete() && !bReportedNoParents)
	{
		bReportedNoParents = true;
		UE_LOG(LogApexSim, Error,
			TEXT("Runtime cars: the car parents under %s are %s — run `-run=ApexMaterialBake` in the editor and re-cook; %s"),
			ApexCarMaterials::Folder, CarParents.Opaque ? TEXT("incomplete") : TEXT("missing"),
			CarParents.Opaque ? TEXT("the rest draw opaque") : TEXT("drawing flat colours"));
	}

	// One instance per slot, owned by this subsystem and shared by every car
	// drawn with the body; see ApexCarContent::OwnMaterialInstance.
	TArray<FName> SlotNames;
	TArray<UMaterialInterface*> SlotMaterials;
	for (const FApexGlbSection& Section : Model.Sections)
	{
		const FApexGlbMaterial& Source = Model.Materials[Section.Material];
		SlotNames.Add(FName(*Source.Name));
		UMaterialInterface* Parent = CarParents.For(Source);
		const FName MidName = MakeUniqueObjectName(this, UMaterialInstanceDynamic::StaticClass(),
			FName(*FString::Printf(TEXT("MI_%s_%s"), *Stem, *ApexCarToml::Segment(Source.Name))));
		UMaterialInstanceDynamic* Mid = nullptr;
		if (Parent)
		{
			Mid = UMaterialInstanceDynamic::Create(Parent, this, MidName);
			Mid->SetVectorParameterValue(ApexCarMaterials::BaseColorFactor, Source.BaseColor);
			if (Textures.IsValidIndex(Source.BaseColorImage) && Textures[Source.BaseColorImage])
			{
				Mid->SetTextureParameterValue(ApexCarMaterials::BaseColorTexture, Textures[Source.BaseColorImage]);
			}
			Mid->SetScalarParameterValue(ApexCarMaterials::MetallicFactor, Source.Metallic);
			Mid->SetScalarParameterValue(ApexCarMaterials::RoughnessFactor, Source.Roughness);
			Mid->SetVectorParameterValue(ApexCarMaterials::EmissiveFactor, Source.Emissive);
			Mid->SetScalarParameterValue(ApexCarMaterials::AlphaCutoff, Source.AlphaCutoff);
			Mid->SetScalarParameterValue(ApexCarMaterials::ClearCoatFactor, Source.ClearCoat);
			Mid->SetScalarParameterValue(ApexCarMaterials::ClearCoatRoughnessFactor, Source.ClearCoatRoughness);
		}
		else if (FallbackMaterial)
		{
			Mid = UMaterialInstanceDynamic::Create(FallbackMaterial, this, MidName);
			Mid->SetVectorParameterValue(TEXT("Color"), Source.BaseColor);
		}
		SlotMaterials.Add(Mid);
	}

	// One vertex instance per source vertex, as the tracks do: the reader
	// already gives every vertex one normal and one UV.
	FMeshDescription Description;
	FStaticMeshAttributes Attributes(Description);
	Attributes.Register();
	TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();
	TVertexInstanceAttributesRef<float> Signs = Attributes.GetVertexInstanceBinormalSigns();
	TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
	TArray<FVector3f> SourceTangents;
	TArray<float> SourceSigns;
	ComputeCarTangents(Model, SourceTangents, SourceSigns);

	const int32 VertexCount = Model.Positions.Num();
	const int32 TriangleCount = Model.NumTriangles();
	Description.ReserveNewVertices(VertexCount);
	Description.ReserveNewVertexInstances(VertexCount);
	Description.ReserveNewTriangles(TriangleCount);
	Description.ReserveNewPolygons(TriangleCount);
	TArray<FVertexInstanceID> Instances;
	Instances.Reserve(VertexCount);
	for (int32 i = 0; i < VertexCount; ++i)
	{
		const FVertexID VertexID = Description.CreateVertex();
		Positions[VertexID] = Model.Positions[i];
		const FVertexInstanceID InstanceID = Description.CreateVertexInstance(VertexID);
		Normals[InstanceID] = Model.Normals[i];
		Tangents[InstanceID] = SourceTangents[i];
		Signs[InstanceID] = SourceSigns[i];
		UVs.Set(InstanceID, 0, Model.UVs[i]);
		Instances.Add(InstanceID);
	}
	// Corners in the reader's order: glTF's counter-clockwise front, through
	// the axis swap, is Unreal's clockwise one (ApexGlbReader.h).
	TArray<FVertexInstanceID> Corners;
	Corners.SetNum(3);
	for (int32 s = 0; s < Model.Sections.Num(); ++s)
	{
		const FApexGlbSection& Section = Model.Sections[s];
		const FPolygonGroupID PolygonGroup = Description.CreatePolygonGroup();
		Attributes.GetPolygonGroupMaterialSlotNames()[PolygonGroup] = SlotNames[s];
		for (int32 Tri = 0; Tri + 2 < Section.Indices.Num(); Tri += 3)
		{
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				Corners[Corner] = Instances[Section.Indices[Tri + Corner]];
			}
			Description.CreatePolygon(PolygonGroup, Corners);
		}
	}

	const FName MeshName = MakeUniqueObjectName(this, UStaticMesh::StaticClass(), FName(*(TEXT("SM_") + Stem)));
	UStaticMesh* Mesh = NewObject<UStaticMesh>(this, MeshName, RF_Transient);
	for (int32 i = 0; i < SlotNames.Num(); ++i)
	{
		// The slot name matches the polygon group's, which is how the build
		// maps sections onto slots, and how the livery and the lights find
		// `car_paint` and `car_brakelight`.
		FStaticMaterial& Slot = Mesh->GetStaticMaterials().Add_GetRef(FStaticMaterial(SlotMaterials[i], SlotNames[i]));
		// The fast build measures no UV density; the textures never stream,
		// so any honest figure will do.
		Slot.UVChannelData.bInitialized = true;
		for (float& Density : Slot.UVChannelData.LocalUVDensities)
		{
			Density = 100.0f;
		}
	}
	// A car collides with nothing on the client (the server owns the
	// physics); say so before the build makes a body setup and tries to cook it.
	Mesh->CreateBodySetup();
	if (UBodySetup* BodySetup = Mesh->GetBodySetup())
	{
		BodySetup->bNeverNeedsCookedCollisionData = true;
	}
	UStaticMesh::FBuildMeshDescriptionsParams BuildParams;
	BuildParams.bFastBuild = true;
	BuildParams.bBuildSimpleCollision = false;
	BuildParams.bCommitMeshDescription = false;
	BuildParams.bMarkPackageDirty = false;
	if (!Mesh->BuildFromMeshDescriptions({&Description}, BuildParams))
	{
		UE_LOG(LogApexSim, Error, TEXT("Car model %s: the mesh build failed"), *Path);
		return nullptr;
	}
	// The fast build's bounds have come out NaN on tracks; the vertices' own
	// box is what the mesh spans, and the cockpit and turntable read it.
	FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
	if (!RenderData || !Model.Bounds.IsValid)
	{
		UE_LOG(LogApexSim, Error, TEXT("Car model %s built empty"), *Path);
		return nullptr;
	}
	RenderData->Bounds = FBoxSphereBounds(FBox(Bounds.IsValid ? Bounds : Model.Bounds));
	Mesh->CalculateExtendedBounds();

	const FVector Size = Mesh->GetBounds().BoxExtent * 2.0;
	UE_LOG(LogApexSim, Log, TEXT("Car model %s: %d tris, %.0f x %.0f x %.0f cm, %d slot(s), %d texture(s), built in %.0f ms"),
		*(FPaths::GetCleanFilename(Path) + Suffix), TriangleCount, Size.X, Size.Y, Size.Z, SlotNames.Num(),
		Textures.FilterByPredicate([](const UTexture2D* T) { return T != nullptr; }).Num(),
		(FPlatformTime::Seconds() - Began) * 1000.0);
	return Mesh;
}

UStaticMesh* ApexCarContent::LoadMesh(const TSoftObjectPtr<UStaticMesh>& Cooked, const FString& RuntimeModel)
{
	if (!RuntimeModel.IsEmpty())
	{
		if (UApexCarContentSubsystem* Content = UApexCarContentSubsystem::Get())
		{
			if (UStaticMesh* Built = Content->LoadModel(RuntimeModel))
			{
				return Built;
			}
		}
	}
	return Cooked.IsNull() ? nullptr : Cooked.LoadSynchronous();
}

FApexCockpitOverrides ApexCarContent::MergeCockpit(const FApexCockpitOverrides* Table, const FApexCockpitOverrides& Toml)
{
	if (!Table)
	{
		return Toml;
	}
	FApexCockpitOverrides Out = *Table;
	auto Take = [](auto& Target, const auto& Value)
	{
		if (!Value.IsZero())
		{
			Target = Value;
		}
	};
	if (Toml.Style != EApexCockpitStyle::Auto)
	{
		Out.Style = Toml.Style;
	}
	Take(Out.Eye, Toml.Eye);
	Take(Out.Wheel, Toml.Wheel);
	Take(Out.MirrorCentre, Toml.MirrorCentre);
	Take(Out.MirrorLeft, Toml.MirrorLeft);
	Take(Out.MirrorRight, Toml.MirrorRight);
	Take(Out.MirrorCentreSizeCm, Toml.MirrorCentreSizeCm);
	Take(Out.MirrorLeftSizeCm, Toml.MirrorLeftSizeCm);
	Take(Out.MirrorRightSizeCm, Toml.MirrorRightSizeCm);
	if (Toml.WheelRakeDeg != 0.0f)
	{
		Out.WheelRakeDeg = Toml.WheelRakeDeg;
	}
	if (Toml.WheelLockDeg > 0.0f)
	{
		Out.WheelLockDeg = Toml.WheelLockDeg;
	}
	Out.bRigWheel = Toml.bRigWheel;
	Out.bRigDash = Toml.bRigDash;
	return Out;
}

UStaticMesh* ApexCarContent::LoadBody(const FApexCarCatalogRow& Row)
{
	return LoadMesh(Row.Mesh, Row.RuntimeModel);
}

UStaticMesh* ApexCarContent::LoadBodyPieces(const FApexCarCatalogRow& Row, TArray<UStaticMesh*>& OutParts)
{
	OutParts.Reset();
	UApexCarContentSubsystem* Content = UApexCarContentSubsystem::Get();
	if (!Row.RuntimeModel.IsEmpty() && !Row.DamageParts.IsEmpty() && Content)
	{
		TArray<UStaticMesh*> Pieces = Content->LoadModelPieces(Row.RuntimeModel, Row.DamageParts);
		if (Pieces.Num() == Row.DamageParts.Num() + 1 && Pieces[0])
		{
			OutParts.Append(Pieces.GetData() + 1, Row.DamageParts.Num());
			return Pieces[0];
		}
	}
	return LoadBody(Row);
}

bool ApexCarContent::HasBody(const FApexCarCatalogRow& Row)
{
	return !Row.RuntimeModel.IsEmpty() || !Row.Mesh.IsNull();
}

UTexture2D* ApexCarContent::LoadLogo(const FApexCarLivery& Livery)
{
	if (!Livery.RuntimeLogo.IsEmpty())
	{
		if (UApexCarContentSubsystem* Content = UApexCarContentSubsystem::Get())
		{
			if (UTexture2D* Loaded = Content->LoadTexture(Livery.RuntimeLogo))
			{
				return Loaded;
			}
		}
	}
	return Livery.Logo.IsNull() ? nullptr : Livery.Logo.LoadSynchronous();
}

UTexture2D* ApexCarContent::LoadLiveryTexture(const FString& RuntimePath)
{
	UApexCarContentSubsystem* Content = RuntimePath.IsEmpty() ? nullptr : UApexCarContentSubsystem::Get();
	return Content ? Content->LoadTexture(RuntimePath) : nullptr;
}

UMaterialInstanceDynamic* ApexCarContent::OwnMaterialInstance(UStaticMeshComponent& Component, int32 Index, bool bFromMesh)
{
	const UStaticMesh* Mesh = Component.GetStaticMesh();
	if (!Mesh || Index < 0 || Index >= Component.GetNumMaterials())
	{
		return nullptr;
	}
	UMaterialInterface* Current = Component.GetMaterial(Index);
	if (!bFromMesh)
	{
		UMaterialInstanceDynamic* Existing = Cast<UMaterialInstanceDynamic>(Current);
		if (Existing && Existing->GetOuter() == &Component)
		{
			return Existing;
		}
	}
	UMaterialInterface* Base = bFromMesh ? Mesh->GetMaterial(Index) : Current;
	if (!Base)
	{
		return nullptr;
	}
	// A dynamic instance cannot parent another: the engine refuses it, and
	// the child draws as the default material. A runtime body's slots are
	// dynamic instances, so the car's own is made from the cooked parent
	// underneath, carrying the model's own values (colour, texture,
	// emission, clear coat) across.
	UMaterialInstanceDynamic* Shared = Cast<UMaterialInstanceDynamic>(Base);
	UMaterialInterface* Parent = Shared ? Shared->Parent.Get() : Base;
	if (!Parent)
	{
		return nullptr;
	}
	UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Parent, &Component);
	if (Shared)
	{
		Instance->CopyParameterOverrides(Shared);
	}
	Component.SetMaterial(Index, Instance);
	return Instance;
}
