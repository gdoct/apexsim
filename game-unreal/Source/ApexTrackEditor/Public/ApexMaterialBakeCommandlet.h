#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "ApexMaterialBakeCommandlet.generated.h"

/**
 * Generates the track parent materials under `/Game/Materials/Track`
 * (`ApexTrackMaterialGraphs`), which both the cooked levels and the tracks
 * the game builds at runtime instantiate.
 *
 * ```
 * UnrealEditor-Cmd.exe <uproject> -run=ApexMaterialBake          # the missing ones
 * UnrealEditor-Cmd.exe <uproject> -run=ApexMaterialBake -force   # all four, again
 * ```
 *
 * `ApexTrackImport` runs the same step before it builds anything, so this is
 * only needed on its own for a build that ships runtime tracks and no
 * imported levels, or after changing a graph.
 */
UCLASS()
class APEXTRACKEDITOR_API UApexMaterialBakeCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UApexMaterialBakeCommandlet();

	virtual int32 Main(const FString& Params) override;
};
