#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "ApexStartupSplashSubsystem.generated.h"

/**
 * Decides when the startup splash may go: when the menu's demo race is
 * running behind it, so the first thing the player sees after the splash is
 * the finished menu over a moving race.
 *
 * The hold itself (a copy of the splash window, and the game window cloaked
 * from the compositor while it loads, resizes and renders) is
 * `ApexStartupSplash` in the ApexSimBoot module, which has to load long before
 * this module can. This claims it and ends it:
 *
 *  - once `AApexRaceDirector::IsDemoReady` holds, fading the splash out over
 *    the menu;
 *  - early when no demo is coming (`UApexDemoModeSubsystem::IsDemoExpected`:
 *    demo off, no server, no track with a level, a refused request);
 *  - after `apexsim.splash.MaxSeconds` in any case.
 *
 * Exclusive fullscreen is not held: a cloaked window cannot take the display.
 */
UCLASS()
class APEXSIM_API UApexStartupSplashSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

private:
	bool Tick(float DeltaSeconds);

	/** Fade the splash off the game window and stop watching. */
	void Reveal(const TCHAR* Why);

	/** Stop watching; the splash is left to whatever already ended it. */
	void StopWatching();

	FTSTicker::FDelegateHandle TickerHandle;
	FDelegateHandle ViewportCreatedHandle;

	/** Seconds held since this subsystem claimed the splash. */
	float HeldFor = 0.0f;
};
