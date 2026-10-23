#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "ApexPortraitSubsystem.generated.h"

class UTexture2D;

/**
 * The driver pictures a profile picks from: `face_<n>.png` in the content's
 * `drivers/portraits` (apexsim-content/drivers/portraits in the editor,
 * `Drivers/portraits` beside ApexSim.exe in a package), read on first use.
 */
UCLASS()
class APEXSIM_API UApexPortraitSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	static UApexPortraitSubsystem* Get(const UObject* WorldContext);

	/** How many pictures there are: `face_1.png` up to the first one missing. */
	int32 Num();

	/** Picture `Index` (1-based), or null for 0, one past the end, or a file that would not read. */
	UTexture2D* GetPortrait(int32 Index);

	/** Where the pictures are read from. */
	static FString Folder();

private:
	void LoadAll();

	UPROPERTY(Transient)
	TArray<TObjectPtr<UTexture2D>> Portraits;

	bool bLoaded = false;
};
