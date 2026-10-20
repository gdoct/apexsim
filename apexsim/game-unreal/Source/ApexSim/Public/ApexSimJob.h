#pragma once

#include "CoreMinimal.h"
#include "Race/ApexPhysicsLib.h"

class FRunnableThread;

/** The tracks and cars the game's own simulation reads (showcases, guides). */
namespace ApexSimContent
{
	/**
	 * `-ApexContentDir=` (a content root holding `tracks/` and `cars/`), else
	 * the content repo in the editor and the server's `content/` beside the
	 * game in a package (<Release>/Server/content). OutCarsDir is its
	 * `cars/default` (the shipped cars only) when there is one. False, with
	 * why, when either folder is missing.
	 */
	APEXSIM_API bool Find(FString& OutTracksDir, FString& OutCarsDir, FString& OutWhy);

	/** A track's YAML under the tracks folder (`default/`, then `custom/`), or empty. */
	APEXSIM_API FString TrackYaml(const FString& TracksDir, const FString& Stem);
}

/**
 * One piece of the game's own simulation (a showcase render, a track guide)
 * on a below-normal thread, polled from the game thread. The work gets a
 * cancel flag it hands to the library. Destroying it cancels the work and
 * waits for its thread.
 */
class APEXSIM_API FApexSimJob : public FNoncopyable
{
public:
	/** The work: read *Cancel now and then, fill OutError on failure. */
	using FWork = TFunction<ApexPhysics::EJobResult(const volatile uint8* Cancel, FString& OutError)>;

	FApexSimJob();
	~FApexSimJob();

	/** Start `Work` under `Label` (for the log); false while another runs. */
	bool Start(const FString& Label, FWork Work);

	bool IsBusy() const { return Run.IsValid(); }
	const FString& GetLabel() const { return Label; }

	/** True once when the work has ended, with how it ended and how long it took. */
	bool Poll(ApexPhysics::EJobResult& OutResult, FString& OutError, double& OutSeconds);

	/** Raise the cancel flag and wait for the thread. */
	void Cancel();

private:
	struct FRun;
	FString Label;
	TUniquePtr<FRun> Run;
	TUniquePtr<FRunnableThread> Thread;
};
