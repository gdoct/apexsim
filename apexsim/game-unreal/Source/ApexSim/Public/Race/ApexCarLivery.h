#pragma once

#include "CoreMinimal.h"

struct FApexCarCatalogRow;
struct FApexCarLivery;
class UStaticMeshComponent;

/**
 * Liveries: repainting a car body at run time.
 *
 * Every generated car GLB carries the same slot names (apexsim-content/docs/cars.md);
 * a livery sets `BaseColorFactor` on `car_paint` and `car_accent` (and
 * `MetallicFactor` on the paint), and `BaseColorTexture` on `car_logo`,
 * through dynamic instances of the body's slot materials (for a car built at
 * runtime, instances of the car parents in ApexCarMaterials.h).
 * A body without one of those slots simply keeps what it has.
 *
 * A texture livery (an imported car's skin) sets `BaseColorTexture` on every
 * `car_skin*` slot (IsSkinSlot) and on each slot it names, and nothing else
 * on them. The slots it swapped are remembered on the component, so the
 * next livery, or livery 0, puts them back. The same call paints the body,
 * the DRS flap and the wheels: an AC skin also recolours the rims.
 */
namespace ApexLivery
{
	/** The livery a roster's index means for this car: null for 0 or one the row does not have. */
	APEXSIM_API const FApexCarLivery* Find(const FApexCarCatalogRow& Row, int32 Livery);

	/**
	 * Paints `Mesh` in `Livery`, or back to the model as authored for null.
	 * Call after the static mesh is set: the overrides are per slot index.
	 */
	APEXSIM_API void Apply(UStaticMeshComponent* Mesh, const FApexCarLivery* Livery);

	/** A slot a skin paints: `car_skin` or `car_skin_<anything>` (`car_skin_1`), any case. */
	APEXSIM_API bool IsSkinSlot(FName Slot);

	/** "Works" for 0, else the livery's name ("<name> (yours)" for the player's own); for the garage. */
	APEXSIM_API FString DisplayName(const FApexCarCatalogRow& Row, int32 Livery);

	/** How many of the row's liveries come from car.toml: the ones the server knows. */
	APEXSIM_API int32 SharedCount(const FApexCarCatalogRow& Row);

	/** The pick as the server should hear it: an own livery goes out as 0, the works. */
	APEXSIM_API int32 ForServer(const FApexCarCatalogRow& Row, int32 Picked);

	/**
	 * What the player's own car wears: the roster's livery, unless the roster
	 * says works and the player picked one of their own, which only this
	 * client has.
	 */
	APEXSIM_API int32 ForLocalCar(const FApexCarCatalogRow& Row, int32 RosterLivery, int32 Picked);

	/**
	 * The player's own livery folders for the car in `CarDir`, in name order:
	 * each `<cars>/custom/liveries/<car folder>/<name>/` holding a livery.toml.
	 */
	APEXSIM_API TArray<FString> OwnLiveryFolders(const FString& CarDir);
}
