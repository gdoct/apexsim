#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "ApexMaterialBakeCommandlet.generated.h"

/**
 * Generates the track parent materials under `/Game/Materials/Track`
 * (`ApexTrackMaterialGraphs`), which the tracks the game builds at runtime
 * instantiate, and the car parents under `/Game/Materials/Car`
 * (`ApexCarMaterials`), which the cars it builds at runtime instantiate.
 *
 * ```
 * UnrealEditor-Cmd.exe <uproject> -run=ApexMaterialBake          # the missing ones
 * UnrealEditor-Cmd.exe <uproject> -run=ApexMaterialBake -force   # all eight, again
 * ```
 *
 * These are the only track and car content that is cooked: every circuit and
 * every car is built at runtime from its files, out of instances of them.
 * `scripts/build_track_levels.ps1` runs this (the missing ones only);
 * `-force` after changing a graph.
 */
UCLASS()
class APEXTRACKEDITOR_API UApexMaterialBakeCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UApexMaterialBakeCommandlet();

	virtual int32 Main(const FString& Params) override;
};
