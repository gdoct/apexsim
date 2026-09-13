#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "ApexPropImportCommandlet.generated.h"

class UMaterialInterface;
class UStaticMesh;
class UTexture;
class UTexture2D;

/**
 * Brings the authored prop kit (`content/props/<kind>/<asset>.glb`, see
 * docs/PROPS.md) into the project as static meshes the track import places.
 *
 * ```
 * UnrealEditor-Cmd.exe <uproject> -run=ApexPropImport -all
 * UnrealEditor-Cmd.exe <uproject> -run=ApexPropImport -kind=barrier,board
 * UnrealEditor-Cmd.exe <uproject> -run=ApexPropImport -asset=barrier/armco_4m
 * ```
 *
 * Options:
 *   -all              every GLB under the source directory
 *   -kind=A,B         every GLB of those kinds
 *   -asset=kind/name  one asset (repeatable, or comma-separated)
 *   -source=DIR       the kit (default: <project>/../content/props)
 *   -dest=PATH        content root (default: /Game/Props)
 *   -dryrun           list what would be imported, write nothing
 *
 * Each GLB becomes `<dest>/<kind>/SM_<asset>` (one mesh, material slots
 * named after the glTF materials — the `<kind>_<surface>` keys the level
 * builder looks for), with its materials and textures under
 * `<dest>/<kind>/Materials`, shared by name across the kind. The ferris
 * wheel is the exception: its `rotor` node is a second mesh,
 * `SM_ferris_wheel_rotor`, so the wheel can turn in-game. The brand and
 * marker PNGs beside the boards become `T_brand_<name>` and `T_marker_<n>`.
 *
 * Re-running replaces assets in place — the package names never change, so
 * the levels that reference them stay valid — and a kind-wide run clears
 * the kind's material folder first so retired materials do not linger.
 * Nothing under `-dest` should be hand-edited.
 */
UCLASS()
class APEXTRACKEDITOR_API UApexPropImportCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UApexPropImportCommandlet();

	virtual int32 Main(const FString& Params) override;

private:
	struct FOptions
	{
		bool bAll = false;
		TArray<FString> Kinds;
		/** `kind/asset` pairs. */
		TArray<FString> Assets;
		FString SourceDir;
		FString DestRoot;
		bool bDryRun = false;
	};

	/** One GLB to import. */
	struct FSource
	{
		FString Kind;
		FString Asset;
		FString Path;
	};

	struct FStats
	{
		int32 Meshes = 0;
		int32 MaterialsImported = 0;
		int32 MaterialsReused = 0;
		int32 TexturesImported = 0;
		int32 TexturesReused = 0;
		int32 Failures = 0;
	};

	static bool ParseOptions(const FString& Params, FOptions& Out, FString& OutError);
	static FString DefaultSourceDir();
	/** The GLBs the options select, sorted for reproducible runs. */
	static bool CollectSources(const FOptions& Options, TArray<FSource>& OutSources, TSet<FString>& OutWholeKinds, FString& OutError);

	/** Import one GLB and settle its meshes, materials and textures where they belong. */
	bool ImportGlb(const FSource& Source, const FOptions& Options, FStats& Stats);

	/**
	 * Move a freshly imported material or texture into the kind's material
	 * folder — or, when one of that name is already there, drop the new one
	 * and point everything imported alongside it at the existing asset.
	 */
	bool SettleMaterials(const FString& MaterialsFolder, const FString& ParentsFolder, TArray<UStaticMesh*>& Meshes,
		TArray<UMaterialInterface*>& Materials, TArray<UTexture*>& Textures, FStats& Stats,
		TSet<UPackage*>& OutPackages, FString& OutError);

	/** The loose PNGs — brands, markers, flags — as sRGB textures. */
	bool ImportLooseTextures(const FOptions& Options, FStats& Stats, const TSet<FString>& WholeKinds);
	static UTexture2D* ImportPng(const FString& PackageName, const FString& PngPath, FString& OutError);

	static bool SavePackages(const TSet<UPackage*>& Packages, FString& OutError);
};
