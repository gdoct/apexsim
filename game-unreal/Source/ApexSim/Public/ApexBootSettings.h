#pragma once

#include "CoreMinimal.h"
#include "GenericPlatform/GenericWindow.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "ApexBootSettings.generated.h"

/**
 * Everything settings.yml carries: enough to get a window on screen and reach
 * a server, and deliberately nothing else.
 *
 * The rest of the settings — assists, quality buckets, key bindings — stay in
 * the binary save slot behind UApexSettingsSubsystem, because they are only
 * ever changed from inside a running game. These few are different: a player
 * whose game opens at the wrong resolution, or on a machine that cannot reach
 * the default server, needs to fix it *before* the game is usable, which means
 * a text file they can edit without the game's help.
 */
struct FApexBootSettings
{
	FIntPoint Resolution = FIntPoint(1920, 1080);

	/** EWindowMode::Type as an int: 0 fullscreen, 1 borderless, 2 windowed. */
	int32 WindowMode = 0;

	bool bVSync = false;

	/** Frames per second; 0 is uncapped. */
	int32 FrameLimit = 144;

	FString ServerHost = TEXT("127.0.0.1");

	int32 ServerPort = 9000;
};

/**
 * Reading and writing the file itself, separated from the subsystem so the
 * round trip can be tested without a game instance.
 */
namespace ApexBootSettingsIo
{
	/**
	 * Fills `InOut` from the file's text. Only a flat two-level subset of YAML
	 * is understood — a `section:` header and `  key: value` lines under it —
	 * which is all the file ever contains. A key that is unknown, or a value
	 * that will not parse, is logged and skipped: whatever `InOut` already held
	 * stands, so one bad line never costs the player the rest of the file.
	 */
	APEXSIM_API void Parse(const FString& Text, FApexBootSettings& InOut);

	/** The whole file, comments and all. Every write regenerates it wholesale. */
	APEXSIM_API FString Serialise(const FApexBootSettings& Settings);
}

/**
 * Owns settings.yml: finds it, reads it at startup, creates it on first run,
 * and writes it back when the values it covers change in-game.
 *
 * The file wins over the save slots for the values it owns. That is the point
 * of it — editing it has to be the way you fix a game that will not start
 * usefully — so UApexSettingsSubsystem and UApexMenuFlowSubsystem both take
 * their copy from here rather than from their own slot, and push changes back
 * so the file never disagrees with what the player last chose in the menus.
 *
 * Both of those subsystems name this one as a dependency, so it is loaded
 * before anything asks it a question.
 */
UCLASS()
class APEXSIM_API UApexBootSettingsSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;

	/** Absolute path of settings.yml on this machine. */
	static FString FilePath();

	const FApexBootSettings& Get() const { return Settings; }

	/**
	 * False on the run that created the file. The subsystems that own these
	 * values use it to decide which way the first copy goes: an existing file
	 * is adopted, a fresh one is seeded from what they already had.
	 */
	bool DidFileExist() const { return bFileExisted; }

	/** Records the display block and rewrites the file if anything moved. */
	void SetDisplay(FIntPoint Resolution, int32 WindowMode, bool bVSync, int32 FrameLimit);

	/** Records the server block and rewrites the file if anything moved. */
	void SetServer(const FString& Host, int32 Port);

private:
	void Write() const;

	FApexBootSettings Settings;
	bool bFileExisted = false;
};
