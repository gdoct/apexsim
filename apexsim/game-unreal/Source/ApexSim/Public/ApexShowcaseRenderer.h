#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"

class FRunnableThread;

/**
 * Showcases rendered by the game itself: the physics library's
 * apx_showcase_render, one at a time on a low-priority thread, into
 * `Saved/ApexSim/Showcases/` (docs/game/spectator.md, "Showcases made in
 * the game"). The file is the one `apexsim-replay showcase` writes for the
 * same showcase.yml entry, tracks and cars, so the menu's backdrop needs
 * neither a server nor a shipped file that is still fresh.
 *
 * Game thread only, apart from the render itself. Destroying it cancels a
 * render under way and waits for its thread (a simulated second at most).
 */
class APEXSIM_API FApexShowcaseRenderer : public FNoncopyable
{
public:
	/** What a render reads: the list and the content it simulates. */
	struct FInputs
	{
		FString ListPath;
		/** Holds `default/<stem>/<stem>.yaml` and the sidecars beside each. */
		FString TracksDir;
		/** The field's car.toml folders (the shipped cars only). */
		FString CarsDir;
	};

	/**
	 * Where the inputs are: `-ApexShowcaseList=` and `-ApexContentDir=`
	 * (a content root holding `tracks/` and `cars/`), else the repo's
	 * showcase.yml and content root in the editor, and in a package the
	 * `showcase.yml` beside ApexSim.exe and the server's `content/`. False,
	 * with the missing piece in OutWhy, when one is not on disk.
	 */
	static bool FindInputs(FInputs& Out, FString& OutWhy);

	/** `Saved/ApexSim/Showcases`, as a full path. */
	static FString CacheDirectory();

	/** The track stem of a showcase name: `Zandervoort.gt3.day` -> `Zandervoort`. */
	static FString StemOf(const FString& Name);

	explicit FApexShowcaseRenderer(const FInputs& InInputs);
	~FApexShowcaseRenderer();

	const FInputs& GetInputs() const { return Inputs; }

	/** Start rendering `Name` into the cache; false while another is under way. */
	bool Start(const FString& Name);

	bool IsBusy() const { return Job.IsValid(); }

	/** The showcase being rendered, empty when idle. */
	const FString& GetCurrent() const { return Current; }

	/**
	 * Collect a finished render: true once per render that ended, with what
	 * it wrote (OutPath, empty when it failed) and why it failed.
	 */
	bool Poll(FString& OutName, FString& OutPath, FString& OutError);

	/** Stop a render under way and wait for its thread. */
	void Cancel();

private:
	struct FJob;

	FInputs Inputs;
	FString Current;
	TUniquePtr<FJob> Job;
	TUniquePtr<FRunnableThread> Thread;
};
