#include "ApexPortraitSubsystem.h"

#include "ApexRepoPaths.h"
#include "ApexSim.h"
#include "Engine/GameInstance.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"

namespace
{
	/** No more pictures than this are looked for. */
	constexpr int32 MaxPortraits = 64;
}

UApexPortraitSubsystem* UApexPortraitSubsystem::Get(const UObject* WorldContext)
{
	const UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	return GameInstance ? GameInstance->GetSubsystem<UApexPortraitSubsystem>() : nullptr;
}

FString UApexPortraitSubsystem::Folder()
{
	// A package carries them beside the game as `Drivers/portraits`
	// (scripts/build_release.ps1); the editor reads the content repo's.
	return FPlatformProperties::RequiresCookedData()
		? FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT(".."), TEXT("Drivers"), TEXT("portraits")))
		: ApexRepoPaths::ContentPath(TEXT("drivers/portraits"));
}

void UApexPortraitSubsystem::LoadAll()
{
	bLoaded = true;
	const FString Dir = Folder();
	for (int32 Index = 1; Index <= MaxPortraits; ++Index)
	{
		const FString Path = FPaths::Combine(Dir, FString::Printf(TEXT("face_%d.png"), Index));
		if (!IFileManager::Get().FileExists(*Path))
		{
			break;
		}
		UTexture2D* Texture = FImageUtils::ImportFileAsTexture2D(Path);
		if (!Texture)
		{
			UE_LOG(LogApexSim, Warning, TEXT("Portraits: %s would not read"), *Path);
		}
		Portraits.Add(Texture);
	}
	UE_LOG(LogApexSim, Log, TEXT("Portraits: %d from %s"), Portraits.Num(), *Dir);
}

int32 UApexPortraitSubsystem::Num()
{
	if (!bLoaded)
	{
		LoadAll();
	}
	return Portraits.Num();
}

UTexture2D* UApexPortraitSubsystem::GetPortrait(int32 Index)
{
	return Index >= 1 && Index <= Num() ? Portraits[Index - 1].Get() : nullptr;
}
