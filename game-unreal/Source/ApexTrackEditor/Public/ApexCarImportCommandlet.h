#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "ApexCarImportCommandlet.generated.h"

class UDataTable;
class UStaticMesh;
struct FApexCarCatalogRow;

/**
 * Brings the cars (`content/cars/<folder>/car.toml` + the GLB it names as
 * `model`) into the project: the mesh as `/Game/Cars/<folder>/SM_<folder>`
 * through Interchange, and a `DT_CarCatalog` row keyed by the car's `id`
 * so the car select, the turntable and the race can find it.
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

	/** What the commandlet reads from a car.toml: the top-level identity plus two physics figures. */
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
	};

	/**
	 * A minimal TOML scan — `key = value` lines under `[table]` headers,
	 * strings, integers and floats, comments stripped — which is all a
	 * car.toml's identity needs. Pure, for the tests.
	 */
	static bool ParseCarToml(const FString& Text, FCarToml& Out, FString& OutError);
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
	UStaticMesh* ImportGlb(const FString& GlbPath, const FString& DestRoot, const FString& Folder, FString& OutError);
	static bool SavePackages(const TSet<UPackage*>& Packages, FString& OutError);
	static void ListRows(const UDataTable& Table, const TArray<FSource>& Sources, const FString& DestRoot);
	/** Drop the rows named by folder or id and delete the mesh folders they point at. */
	static int32 RemoveCars(UDataTable& Table, const TArray<FString>& Names, const FOptions& Options);
};
