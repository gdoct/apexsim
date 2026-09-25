#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "Race/ApexReplayCamera.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"

#include "ApexReplaySubsystem.generated.h"

class AApexRaceDirector;
class FApexReplayClip;

/**
 * Plays a race clip from disk and, optionally, renders it to numbered PNG
 * frames: the capability the promo video is cut from.
 *
 * Exists only in a run started with `-ApexReplay=<file>.clip.json` (a clip
 * cut by the server's `apexsim-replay` tool). That run never connects to a
 * server or plays the menu's demo: it waits for the menu world, hands the
 * clip to the race director (BeginReplayView), hides the shell, waits for
 * the track level and its sky, holds a moment for the lighting to settle,
 * then rolls the clip's clock.
 *
 * With `-ApexReplayRecord=<dir>` every frame from `-ApexReplayStart` for
 * `-ApexReplayDuration` seconds is written as `<dir>/frame_000000.png`… at
 * a fixed timestep of `-ApexReplayFps` (60), so the result is exact however
 * slowly the machine renders; `<dir>/replay_done.json` says what was
 * written, and the game quits (unless `-ApexReplayNoExit`).
 *
 * The camera: `-ApexReplayCam=tv|fixed|pan|chase|cockpit` (ApexReplayCam),
 * `-ApexReplayFollow=leader|nearest|<car index>`, `-ApexReplayShot=<tv shot>`,
 * `-ApexReplayLockTv`, `-ApexCamera=` / `-ApexCameraLookAt=` / `-ApexCameraFov=`
 * for the tripod (server frame, as for the screenshot camera),
 * `-ApexReplayLook=X,Y,Z` / `-ApexReplayLookBias=` / `-ApexReplayFrameWidth=`
 * / `-ApexReplayPanSpeed=` for a pan, `-ApexReplayChase=roof|close|near|far`.
 * The sky is the clip's; `-ApexReplayTimeOfDay=hh:mm` / `-ApexReplayWeather=`
 * override the look (not the grip the race was simulated with).
 * `docs/PROMO_VIDEO.md` has the whole pipeline.
 */
UCLASS()
class APEXSIM_API UApexReplaySubsystem : public UGameInstanceSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	/** The command line asks for a replay. */
	static bool IsReplayRun();

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override { return Phase != EPhase::Idle && Phase != EPhase::Done; }
	virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Conditional; }
	virtual bool IsTickableWhenPaused() const override { return true; }

private:
	enum class EPhase : uint8
	{
		Idle,
		WaitWorld,
		Loading,
		Warmup,
		Playing,
		Finishing,
		Done,
	};

	/** Read every `-ApexReplay*` switch; false when the clip cannot be played. */
	bool ParseCommandLine();
	void ApplyResolution();
	/** Collapse the menu shell and toasts; the replay is the whole screen. */
	void HideShell();
	void RequestFrame();
	void WriteManifest();
	void Fail(const FString& Why);
	AApexRaceDirector* GetDirector() const;

	EPhase Phase = EPhase::Idle;
	float PhaseSeconds = 0.0f;
	int32 PhaseFrames = 0;

	TSharedPtr<FApexReplayClip> Clip;
	FString ClipPath;
	ApexReplayCam::FSettings Camera;
	FApexSessionConditions Conditions;

	/** Clip seconds to film from and how long (<= 0: to the end). */
	double StartSeconds = 0.0;
	double DurationSeconds = 0.0;
	double EndSeconds = 0.0;
	/** The clock rolls this long before StartSeconds, unrecorded, so motion blur and the cameras have history. */
	double PrerollSeconds = 1.0;
	/** Held on the first frame once everything is in, for exposure and streaming to settle. */
	float WarmupSeconds = 3.0f;
	float LoadTimeoutSeconds = 120.0f;
	bool bLoop = false;
	bool bShowUi = false;

	FString RecordDir;
	int32 Fps = 60;
	int32 FrameIndex = 0;
	int32 TotalFrames = 0;
	FIntPoint Resolution = FIntPoint::ZeroValue;
	bool bExitWhenDone = true;
	double RecordStartedAt = 0.0;
};
