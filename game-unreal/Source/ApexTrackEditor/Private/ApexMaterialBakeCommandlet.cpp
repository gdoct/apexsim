#include "ApexMaterialBakeCommandlet.h"

#include "ApexTrackEditorModule.h"
#include "ApexTrackMaterialGraphs.h"

UApexMaterialBakeCommandlet::UApexMaterialBakeCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
}

int32 UApexMaterialBakeCommandlet::Main(const FString& Params)
{
	TArray<FString> Tokens;
	TArray<FString> Switches;
	ParseCommandLine(*Params, Tokens, Switches);
	const bool bForce = Switches.Contains(TEXT("force"));

	UE_LOG(LogApexTrackImport, Display, TEXT("Baking the track parent materials into %s%s"),
		ApexTrackMaterials::Folder, bForce ? TEXT(" (all of them)") : TEXT(""));
	FString Error;
	if (!ApexTrackMaterialGraphs::Bake(bForce, Error))
	{
		UE_LOG(LogApexTrackImport, Error, TEXT("%s"), *Error);
		return 1;
	}
	return 0;
}
