#pragma once

#include "CoreMinimal.h"
#include "Track/ApexTrackSceneBuilder.h"

/**
 * The four parent materials every track instantiates, generated as graphs
 * and saved as assets under `/Game/Materials/Track` (`M_ApexTrackBase`,
 * `M_ApexEmissive`, `M_ApexBrand`, `M_ApexDecal`). Cooked, they are what the
 * game's dynamic instances are made from when it builds a circuit from its
 * export; the editor's inspection import (`ApexTrackImport`) makes instance
 * constants of the same. Nothing is hand-authored: re-run
 * `-run=ApexMaterialBake -force` after changing a graph here.
 */
namespace ApexTrackMaterialGraphs
{
	/**
	 * Generate and save the parents that are missing — all of them with
	 * `bForce` — and `M_ApexTrackBase` again whenever the ground textures
	 * have been imported (or removed) since it was baked, since which of its
	 * two surface graphs it carries depends on them.
	 */
	bool Bake(bool bForce, FString& OutError);

	/** `Bake(false)`, then load the parents. */
	bool EnsureParents(FApexTrackParents& Out, FString& OutError);

	/** Whether `/Game/Ground` holds the asphalt set, i.e. the base parent should sample the baked maps. */
	bool GroundSetImported();
}	 // namespace ApexTrackMaterialGraphs
