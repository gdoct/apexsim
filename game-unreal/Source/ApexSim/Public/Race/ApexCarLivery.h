#pragma once

#include "CoreMinimal.h"

struct FApexCarCatalogRow;
struct FApexCarLivery;
class UStaticMeshComponent;

/**
 * Liveries: repainting a car body at run time.
 *
 * Every generated car GLB carries the same slot names (docs/CAR_MODELS.md);
 * a livery sets `BaseColorFactor` on `car_paint` and `car_accent` (and
 * `MetallicFactor` on the paint), and `BaseColorTexture` on `car_logo`,
 * through dynamic instances of the Interchange glTF materials the import made.
 * A body without one of those slots simply keeps what it has.
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

	/** "Works" for 0, else the livery's name; for the garage. */
	APEXSIM_API FString DisplayName(const FApexCarCatalogRow& Row, int32 Livery);
}
