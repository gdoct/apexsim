#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"
#include "Catalog/ApexCatalogRows.h"
#include "Subsystems/EngineSubsystem.h"

#include "ApexCarContentSubsystem.generated.h"

class IImageWrapperModule;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UStaticMesh;
class UStaticMeshComponent;
class UTexture2D;
struct FApexGlbImage;
struct FApexGlbModel;

/**
 * Where the game's cars come from: their files on disk, as the tracks do.
 *
 * A car is its folder — `car.toml` plus the GLB it names as `model`, the
 * DRS flap GLB, the livery logos and skins, and any wheel or steering wheel
 * GLB of its own — and otherwise the class wheel `<model>.glb` in the wheels
 * folder beside the cars. The game reads each
 * car.toml when a catalog row is first asked for, and builds a GLB into a
 * transient static mesh the first time something draws it: its own GLB
 * reader (`ApexGlb`), the fast mesh build the tracks use, and dynamic
 * instances of the cooked car parents (`ApexCarMaterials`). Nothing about a
 * car is cooked: a new car is its folder dropped in the cars folder.
 *
 * The folders, in order (the first to hold an id wins):
 *  - `-ApexCarsDir=<dir>` on the command line (several joined with `+`);
 *  - a packaged build: `Cars/` beside `ApexSim.exe` (`<Release>/Game/Cars`),
 *    wheels in `Wheels/` beside it;
 *  - the editor: the repo's `content/cars`, wheels in `content/wheels`.
 * Each is read as `default/` (the shipped cars) then `custom/` (the
 * player's own), or as it is when it has neither (`CarFolders`), so a custom
 * car reusing a shipped id is the one left out. The wheels folder sits
 * beside the cars folder, not beside `default/`.
 *
 * A row from a car on disk wins over the `DT_CarCatalog` table's; the table
 * is a fallback for a car with no folder here, and lends a runtime car its
 * hand-tuned turntable framing and cockpit points when the car.toml has no
 * `[preview]` / `[cockpit]` table.
 *
 * An engine subsystem, because a car's meshes belong to no world: the wheel
 * and flap code that loads them has none to hand, and a built car outlives
 * the demo's world into the race's. Built meshes are kept until
 * `apexsim.car.Rescan`.
 */
UCLASS()
class APEXSIM_API UApexCarContentSubsystem : public UEngineSubsystem
{
	GENERATED_BODY()

public:
	/** The engine's instance; null before the engine is up. */
	static UApexCarContentSubsystem* Get();

	virtual void Deinitialize() override;

	/** Read the car folders again and drop every built car (`apexsim.car.Rescan`). */
	void Rescan();

	/** The folders scanned for cars, in order. */
	static TArray<FString> CarDirectories();

	/**
	 * The folders one cars folder's cars sit in, in order: its `default` and
	 * `custom` subfolders, or the folder itself when it has neither.
	 */
	static TArray<FString> CarFolders(const FString& CarsDir);

	/**
	 * The catalog row for a car id: the car on disk's, else the table's;
	 * null when neither knows it. Case-insensitive.
	 */
	const FApexCarCatalogRow* FindRow(const FString& CarId) const;

	/** Every row, runtime ones first, then table rows no runtime car has replaced. */
	void ForEachRow(TFunctionRef<void(const FString& CarId, const FApexCarCatalogRow& Row)> Fn) const;

	int32 NumRuntimeCars() const;

	/**
	 * The mesh built from a GLB, built now if it has not been. Null, logged
	 * once, when the file cannot be read or built.
	 */
	UStaticMesh* LoadModel(const FString& Path);

	/**
	 * A PNG or JPEG (a livery logo, skin or slot texture) as a transient sRGB
	 * texture, loaded once per path: twenty cars in one skin share it. Null,
	 * logged once, when it will not decode.
	 */
	UTexture2D* LoadTexture(const FString& Path);

	/**
	 * Start reading a row's GLBs (body, wheels, flap, steering wheel) on the thread pool, so
	 * the `LoadModel` that follows only builds. The race director calls it for
	 * the whole roster before it dresses the first car.
	 */
	void Prefetch(const FApexCarCatalogRow& Row);

private:
	/** A GLB read and its images decoded, off the game thread. */
	struct FParsedModel
	{
		TSharedPtr<FApexGlbModel> Model;
		FString Error;
		FString Warning;
		double Seconds = 0.0;
		bool bOk = false;
	};

	void EnsureScanned() const;
	void ScanNow();
	void DropBuilt();
	static FString ModelKey(const FString& Path);
	static TSharedPtr<FParsedModel> ParseModel(const FString& Path, IImageWrapperModule* ImageWrappers);
	UStaticMesh* BuildModel(const FString& Path, const FApexGlbModel& Model);
	UTexture2D* MakeTexture(const FApexGlbImage& Image, const FString& Name);
	IImageWrapperModule* ImageWrappers();

	bool bScanned = false;
	/** Car id (lower case) -> row, from the folders. */
	TMap<FString, FApexCarCatalogRow> RuntimeRows;
	/** Car id (lower case) -> row, from `DT_CarCatalog`. */
	TMap<FString, FApexCarCatalogRow> TableRows;

	/** GLB path (full, lower case) -> built mesh. */
	UPROPERTY(Transient)
	TMap<FString, TObjectPtr<UStaticMesh>> Models;

	/** PNG path (full, lower case) -> texture. */
	UPROPERTY(Transient)
	TMap<FString, TObjectPtr<UTexture2D>> Images;

	/** The cooked parents the instances are made from, held while cars use them. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInterface>> Parents;

	/** Flat colours when the parents were never baked. */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> FallbackMaterial;

	bool bParentsLoaded = false;
	bool bReportedNoParents = false;

	/** Reads in flight, by model key. */
	TMap<FString, TFuture<TSharedPtr<FParsedModel>>> Pending;
	/** Model keys that failed this run; cleared by a rescan. */
	TSet<FString> Broken;
};

/**
 * Where a catalog row's assets come from, runtime file or cooked asset,
 * for everything that draws a car. Rows made from a car on disk carry file
 * paths (`RuntimeModel`, `RuntimeLogo`); table rows carry soft pointers.
 */
namespace ApexCarContent
{
	/**
	 * A car.toml's `[cockpit]` over the catalog table's row for the car:
	 * every point, size, rake and lock the TOML leaves at zero (and an Auto
	 * style) keeps the table's; the rig flags are the TOML's.
	 */
	APEXSIM_API FApexCockpitOverrides MergeCockpit(const FApexCockpitOverrides* Table, const FApexCockpitOverrides& Toml);

	/** The body a row draws: its runtime model, else its cooked mesh; null for neither. */
	APEXSIM_API UStaticMesh* LoadBody(const FApexCarCatalogRow& Row);

	/** Whether the row has a body to draw at all, without building it. */
	APEXSIM_API bool HasBody(const FApexCarCatalogRow& Row);

	/** A wheel or flap: the runtime model when there is one, else the cooked mesh. */
	APEXSIM_API UStaticMesh* LoadMesh(const TSoftObjectPtr<UStaticMesh>& Cooked, const FString& RuntimeModel);

	/** A livery's logo, runtime PNG or cooked texture; null when it has none. */
	APEXSIM_API UTexture2D* LoadLogo(const FApexCarLivery& Livery);

	/** A livery's skin or slot texture (`RuntimeSkin`, `RuntimeTextures`); null for an empty path or one that will not load. */
	APEXSIM_API UTexture2D* LoadLiveryTexture(const FString& RuntimePath);

	/**
	 * A dynamic instance on slot `Index` that belongs to `Component` alone.
	 *
	 * A runtime car's slots are already dynamic instances, shared by every
	 * car drawn with that body, so the component helpers
	 * (`CreateDynamicMaterialInstance`, `CreateAndSetMaterialInstanceDynamic`)
	 * would hand back the shared one and repaint every such car at once. Nor
	 * can the shared one be a parent: the engine accepts only a material or a
	 * material instance constant, and a dynamic instance under another draws
	 * as the default material. So this makes an instance of the shared one's
	 * cooked parent with its values copied (or, for a cooked body, of the
	 * slot's material) — of whatever the slot shows, or with `bFromMesh` of
	 * the mesh's own material, dropping any earlier override — unless the
	 * slot already shows one owned by `Component`.
	 */
	APEXSIM_API UMaterialInstanceDynamic* OwnMaterialInstance(UStaticMeshComponent& Component, int32 Index, bool bFromMesh = false);
}
