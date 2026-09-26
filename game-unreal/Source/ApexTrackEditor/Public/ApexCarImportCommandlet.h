#pragma once

#include "CoreMinimal.h"
#include "Cars/ApexCarToml.h"
#include "Catalog/ApexCatalogRows.h"
#include "Commandlets/Commandlet.h"

#include "ApexCarImportCommandlet.generated.h"

class UDataTable;
class UStaticMesh;

/**
 * Brings the cars (`content/cars/<folder>/car.toml` + the GLB it names as
 * `model`) into the project: the mesh as `/Game/Cars/<folder>/SM_<folder>`
 * through Interchange, and a `DT_CarCatalog` row keyed by the car's `id`
 * so the editor can show it.
 *
 * The game does not use either: it builds every car from its folder at
 * runtime (`UApexCarContentSubsystem`), and `/Game/Cars` is never cooked.
 * The table's hand-tuned fields (turntable framing, cockpit points) are
 * still read, for a car whose car.toml has no `[preview]` / `[cockpit]`.
 *
 * The bodies carry no wheels. A car's `[wheels]` table names a shared wheel,
 * `content/wheels/<model>.glb`, imported once per run that needs it as
 * `/Game/Cars/Wheels/<model>/SM_Wheel_<model>`, and gives the axles; both
 * go on the row as `Wheels`, which is derived and so kept in step with the
 * TOML on every run, like the checksum. So is `EngineSound`: the `[sound]`
 * table (cylinders, crank, exhaust, pops...) and the `[engine]` rev range the
 * client's engine synthesiser is built from (Audio/ApexEngineSound.h).
 *
 * ```
 * UnrealEditor-Cmd.exe <uproject> -run=ApexCarImport -all
 * UnrealEditor-Cmd.exe <uproject> -run=ApexCarImport -car=yotota-lmp2,posh-lmp2
 * UnrealEditor-Cmd.exe <uproject> -run=ApexCarImport -list
 * ```
 *
 * Options:
 *   -all              every car folder with a car.toml
 *   -car=A,B          those folders
 *   -list             print the catalog rows against the car folders and stop
 *   -remove=A,B       delete those cars' catalog rows and their mesh folders
 *                     under -dest (by folder name or row id) and stop
 *   -force            re-import the mesh and rewrite the row's fields from the
 *                     TOML (the preview and cockpit tweaks are kept)
 *   -source=DIR       the cars (default: <project>/../content/cars)
 *   -wheels=DIR       the shared wheels (default: <source>/../wheels)
 *   -dest=PATH        mesh root (default: /Game/Cars)
 *   -table=PATH       the catalog (default: /Game/Data/DT_CarCatalog)
 *   -dryrun           report without writing
 *
 * Without -force the pass is additive: a car with no mesh asset gets one, a
 * car with no row gets one, and a row whose mesh is missing or belongs to
 * another car's folder is pointed at this car's mesh. Everything else on an
 * existing row — preview framing, cockpit points, hand-tuned fields — is
 * left alone.
 */
UCLASS()
class APEXTRACKEDITOR_API UApexCarImportCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UApexCarImportCommandlet();

	virtual int32 Main(const FString& Params) override;

	/** The car.toml's client side, read by the runtime's parser (Cars/ApexCarToml.h). */
	using FWheelsToml = FApexCarWheelsToml;
	using FDrsFlapToml = FApexCarDrsFlapToml;
	using FLiveryToml = FApexCarLiveryToml;
	using FCarToml = FApexCarToml;

	/** `ApexCarToml::Parse`: the game reads car.toml with the same code. */
	static bool ParseCarToml(const FString& Text, FCarToml& Out, FString& OutError)
	{
		return ApexCarToml::Parse(Text, Out, OutError);
	}

	/** `f1` -> `/Game/Cars/Wheels/f1/SM_Wheel_f1` under `DestRoot`, as a package name. */
	static FString WheelPackageName(const FString& DestRoot, const FString& Model);
	/** The row's wheel figures from the TOML, pointing at `Mesh`; an empty spec when the TOML has none. */
	static FApexWheelSpec MakeWheelSpec(const FCarToml& Toml, const TSoftObjectPtr<UStaticMesh>& Mesh);
	/** `fugazzi-sf26` -> `/Game/Cars/fugazzi_sf26/Drs/SM_fugazzi_sf26_drs`, as a package name. */
	static FString DrsFlapPackageName(const FString& DestRoot, const FString& Folder);
	/** The row's DRS flap from the TOML, pointing at `Mesh`; an empty spec when the TOML has none. */
	static FApexDrsFlapSpec MakeDrsFlapSpec(const FCarToml& Toml, const TSoftObjectPtr<UStaticMesh>& Mesh);
	/** `textures/blue_logo.png` -> `/Game/Cars/<folder>/Liveries/T_blue_logo`, as a package name. */
	static FString LiveryLogoPackageName(const FString& DestRoot, const FString& Folder, const FString& Logo);
	/** `yotota-lmp2` -> `yotota_lmp2`: a folder name as a package name segment. */
	static FString PackageSegment(const FString& Folder);

private:
	struct FOptions
	{
		bool bAll = false;
		bool bList = false;
		TArray<FString> Remove;
		bool bForce = false;
		bool bDryRun = false;
		TArray<FString> Cars;
		FString SourceDir;
		FString WheelSourceDir;
		FString DestRoot;
		FString TablePath;
	};

	struct FSource
	{
		FString Folder;
		FString TomlPath;
		FCarToml Toml;
		/** The TOML's checksum as the server computes it (ApexContentCrc.h). */
		int64 SourceCrc = 0;
	};

	static bool ParseOptions(const FString& Params, FOptions& Out, FString& OutError);
	static bool CollectSources(const FOptions& Options, TArray<FSource>& OutSources, FString& OutError);
	static FString MeshPackageName(const FString& DestRoot, const FString& Folder);
	static FString MeshObjectPath(const FString& DestRoot, const FString& Folder);

	/** The mesh for a car: the existing asset unless -force, else imported from the GLB. */
	UStaticMesh* ResolveMesh(const FSource& Source, const FOptions& Options, TSet<UPackage*>& OutPackages, FString& OutError);
	/**
	 * The shared wheel a car names: the existing asset unless -force (which
	 * re-imports each wheel once per run), else imported. Null with no error
	 * for a dry run.
	 */
	UStaticMesh* ResolveWheelMesh(const FString& Model, const FOptions& Options, TSet<UPackage*>& OutPackages, FString& OutError);
	/**
	 * A car's DRS flap mesh: the existing asset unless -force, else imported
	 * from the GLB its `[drs_flap]` names, into its own folder so its
	 * materials do not land on the body's. Null with no error for a dry run.
	 */
	UStaticMesh* ResolveDrsFlapMesh(const FSource& Source, const FOptions& Options, TSet<UPackage*>& OutPackages, FString& OutError);
	/** The row's liveries from the TOML, importing each logo PNG once (again under -force). */
	bool ResolveLiveries(const FSource& Source, const FOptions& Options, TSet<UPackage*>& OutPackages,
		TArray<FApexCarLivery>& OutLiveries, FString& OutError);
	static class UTexture2D* ImportTexture(const FString& PngPath, const FString& PackageName, FString& OutError);
	/** One GLB as one combined mesh, `PackageName` (e.g. /Game/Cars/x/SM_x), materials beside it. */
	UStaticMesh* ImportGlb(const FString& GlbPath, const FString& PackageName, FString& OutError);
	/** Adds every dirty package under `Folder` (a long package path) to `OutPackages`. */
	static void CollectDirtyPackages(const FString& Folder, TSet<UPackage*>& OutPackages);
	static bool SavePackages(const TSet<UPackage*>& Packages, FString& OutError);
	static void ListRows(const UDataTable& Table, const TArray<FSource>& Sources, const FString& DestRoot);
	/** Drop the rows named by folder or id and delete the mesh folders they point at. */
	static int32 RemoveCars(UDataTable& Table, const TArray<FString>& Names, const FOptions& Options);

	/** Wheels resolved this run, by model: each is imported at most once. */
	UPROPERTY(Transient)
	TMap<FString, TObjectPtr<UStaticMesh>> WheelMeshes;
};
