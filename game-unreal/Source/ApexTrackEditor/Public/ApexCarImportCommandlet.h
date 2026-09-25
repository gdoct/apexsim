#pragma once

#include "CoreMinimal.h"
#include "Catalog/ApexCatalogRows.h"
#include "Commandlets/Commandlet.h"

#include "ApexCarImportCommandlet.generated.h"

class UDataTable;
class UStaticMesh;

/**
 * Brings the cars (`content/cars/<folder>/car.toml` + the GLB it names as
 * `model`) into the project: the mesh as `/Game/Cars/<folder>/SM_<folder>`
 * through Interchange, and a `DT_CarCatalog` row keyed by the car's `id`
 * so the car select, the turntable and the race can find it.
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
 * left alone. The four cars that were imported by hand before this
 * commandlet existed keep their meshes: their rows already point at them.
 */
UCLASS()
class APEXTRACKEDITOR_API UApexCarImportCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UApexCarImportCommandlet();

	virtual int32 Main(const FString& Params) override;

	/** A car.toml's `[wheels]` table: where the client draws the wheels, metres. */
	struct FWheelsToml
	{
		/** `content/wheels/<Model>.glb`; empty when the table is absent. */
		FString Model;
		float FrontAxleM = 0.0f;
		float RearAxleM = 0.0f;
		float FrontTrackM = 0.0f;
		float RearTrackM = 0.0f;
		float FrontRadiusM = 0.0f;
		float RearRadiusM = 0.0f;
		float FrontWidthM = 0.0f;
		float RearWidthM = 0.0f;

		bool IsPresent() const { return !Model.IsEmpty(); }
	};

	/** The `[drs_flap]` table: the flap GLB beside the car.toml, its hinge and its travel. */
	struct FDrsFlapToml
	{
		/** Relative to the car folder; empty when the table is absent. */
		FString Model;
		float HingeForwardM = 0.0f;
		float HingeUpM = 0.0f;
		float OpenDeg = 0.0f;

		bool IsPresent() const { return !Model.IsEmpty(); }
	};

	/** A `[[livery]]` table: colours are linear RGB, `logo` a PNG relative to the car folder. */
	struct FLiveryToml
	{
		FString Name;
		FLinearColor Paint = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
		/** Zero alpha when the table names no accent: the model's is kept. */
		FLinearColor Accent = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
		float Metallic = -1.0f;
		FString Logo;
	};

	/** What the commandlet reads from a car.toml: the identity, a few physics figures and the wheels. */
	struct FCarToml
	{
		FString Id;
		FString Name;
		FString Model;
		FString Brand;
		FString CarClass;
		FString ManufacturerCountry;
		int32 ModelYear = 0;
		float MassKg = 0.0f;
		float MaxPowerKw = 0.0f;
		float MaxSteerRad = 0.0f;
		FWheelsToml Wheels;
		FDrsFlapToml DrsFlap;
		/**
		 * The `[sound]` table and the `[engine]` rev range, already in the
		 * row's shape: derived like the wheels, so it follows the TOML on
		 * every run. `Cylinders == 0` when the car has no `[sound]` table.
		 */
		FApexEngineSoundSpec Sound;
		/** The `[[livery]]` tables, in order. */
		TArray<FLiveryToml> Liveries;
	};

	/**
	 * A minimal TOML scan — `key = value` lines under `[table]` headers,
	 * strings, integers and floats, comments stripped — which is all a
	 * car.toml's identity needs. Pure, for the tests.
	 */
	static bool ParseCarToml(const FString& Text, FCarToml& Out, FString& OutError);
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
