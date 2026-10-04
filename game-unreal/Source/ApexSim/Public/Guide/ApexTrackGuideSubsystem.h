#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"
#include "Guide/ApexGuidePlayer.h"
#include "Guide/ApexTrackGuide.h"
#include "Race/ApexReplayCamera.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"

#include "ApexTrackGuideSubsystem.generated.h"

class AApexRaceDirector;
class FApexReplayClip;

/** A guide file on disk, from its name alone. */
struct FApexGuideFileInfo
{
	FString Stem;
	FString Class;
	/** The `.guide.json`; the recording is named inside it. */
	FString Path;
};

/** Where an open guide is. */
enum class EApexGuideState : uint8
{
	/** No guide open. */
	Idle,
	/** Reading the recording off the game thread. */
	Loading,
	/** On the director, waiting for the track and the sky. */
	Building,
	/** Playing. */
	Running,
};

/** A camera of the guide's cycle: a tripod from the file, or one of the client's own on car 0. */
enum class EApexGuideCameraKind : uint8
{
	Fixed,
	/** The broadcast director, locked on car 0. */
	Broadcast,
	Chase,
	Onboard,
};

/**
 * The track guide (docs/TRACK_GUIDE.md): a corner-by-corner walk round a
 * circuit, played from files made offline. No server, no session.
 *
 * Finds the guides (`-ApexGuideDir=`, `Guide/` beside ApexSim.exe in a
 * package, the repo's `build/guide` in the editor; first folder wins per
 * stem and class), and plays one: reads the `.guide.json`, inflates its
 * recording on the thread pool, puts it on the race director's guide view
 * (AApexRaceDirector::BeginGuideView, which ends the menu's backdrop; it
 * comes back by itself when the guide closes) and from then on owns the
 * recording's clock through ApexGuide::FPlayer: the overview frozen at the
 * line, the fast-forward to a corner, the corner's loop at normal speed and
 * in slow motion.
 *
 * Cameras: each corner's tripods from the file, kept only if the client's
 * trace finds them clear of the ground and with a sight line to their look
 * point, then the broadcast director locked on car 0, a chase camera and the
 * onboard. A tripod turns a little toward car 0 but keeps its corner in frame.
 * A car-relative camera chosen stays chosen from corner to corner; a tripod
 * choice starts over on the next corner's first.
 *
 * The shell (UApexRootWidget) listens to OnActiveChanged to hide the menu
 * and show UApexTrackGuideWidget, and routes the guide's keys to Run.
 *
 * Console: `apexsim.guide.Open <Stem> [Class]`, `.Next`, `.Prev`,
 * `.Corner N`, `.Camera [N]`, `.Pause`, `.Close`, `.Rescan`. Command line:
 * `-ApexGuide=<Stem>[:Class]` (opens once the menu is up),
 * `-ApexGuideCorner=N`, `-ApexGuideCamera=N`.
 */
UCLASS()
class APEXSIM_API UApexTrackGuideSubsystem : public UGameInstanceSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override { return State != EApexGuideState::Idle; }
	virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Conditional; }
	virtual bool IsTickableWhenPaused() const override { return true; }

	/** The folders searched, first first. */
	static TArray<FString> GuideDirectories();

	/** Read the folders again (`apexsim.guide.Rescan`). */
	void Rescan();

	/** Every guide found, in the order the folders were searched. */
	const TArray<FApexGuideFileInfo>& GetFiles() const { return Files; }

	/** The track has a guide of some class, and this machine has its export to build it from. */
	bool HasGuide(const FString& Stem) const;

	/** The guide `Open` would play for a track (see ApexTrackGuide::ChooseClass), or null. */
	const FApexGuideFileInfo* FindGuide(const FString& Stem, const FString& PreferredClass) const;

	/** The class of the car picked on the create screen, or empty. */
	FString GetPreferredClass() const;

	/**
	 * Open a track's guide (of `Class`, else the preferred one). False with a
	 * reason when there is none, it does not parse or its track is not
	 * installed; the recording's own failures come later, through OnFailed.
	 */
	bool Open(const FString& Stem, const FString& Class, FString& OutError);
	/** Back to the menu. */
	void Close();

	bool IsOpen() const { return State != EApexGuideState::Idle; }
	EApexGuideState GetState() const { return State; }
	/** 0 until the track is in and lit, rising to 1 as it is revealed. */
	float GetReveal() const { return Reveal; }

	// --- Controls --------------------------------------------------------------

	void Next();
	void Previous();
	void ShowOverview();
	/** A corner by its 1-based place in the lap; `bCut` jumps rather than fast-forwarding. */
	void GoToCorner(int32 Number, bool bCut = false);
	void TogglePause();
	/** The next camera of the cycle (Delta -1: the one before). */
	void CycleCamera(int32 Delta = 1);
	/** A camera by its 1-based place in the cycle, as the readout numbers them. */
	void SetCamera(int32 Number);
	/** Carry out a key's action (ApexGuide::CommandFor). */
	void Run(const ApexGuide::FCommand& Command);

	// --- What the widget shows -----------------------------------------------------

	const FApexTrackGuide& GetGuide() const { return Guide; }
	const ApexGuide::FPlayer& GetPlayer() const { return Player; }
	/** The corner on screen or being travelled to, or null at the overview. */
	const FApexGuideCorner* GetCorner() const;
	FString GetCameraName() const;
	int32 GetCameraNumber() const { return CameraIndex + 1; }
	int32 GetCameraCount() const { return Cameras.Num(); }
	/** The guide car's display name (the file's, else its catalog row's). */
	FString GetCarName() const;

	/** -ApexGuide= and its options, once the menu world is up. The shell calls this every frame until it is done. */
	void ApplyCommandLine();

	/** Opened (true) or closed (false). */
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnActiveChanged, bool);
	FOnActiveChanged OnActiveChanged;
	/** A guide that could not be played after all, with the reason; it has closed. */
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnFailed, const FString&);
	FOnFailed OnFailed;

private:
	struct FCamera
	{
		FString Name;
		EApexGuideCameraKind Kind = EApexGuideCameraKind::Chase;
		FVector EyeCm = FVector::ZeroVector;
		FVector LookCm = FVector::ZeroVector;
		float FovDeg = 50.0f;
		/** Tripods are checked against the track once, when first wanted. */
		bool bChecked = false;
		bool bUsable = true;
	};

	AApexRaceDirector* GetDirector() const;
	void Fail(const FString& Why);
	/** The cameras of the overview or of a corner, then the client's own. */
	void BuildCameras();
	/** Put the camera at CameraIndex (or the next usable one) on the director. */
	void ApplyCamera();
	/** Check a tripod against the track; false when it cannot be used. */
	bool CheckCamera(FCamera& Camera) const;
	/** Whichever usable camera is `Delta` steps from `From`, wrapping; INDEX_NONE if none. */
	int32 StepCamera(int32 From, int32 Delta);
	/** What the clock says now goes to the director. */
	void PushClock();
	/** The overview or a new corner came up (or the travel to it began): its cameras. Then the clock. */
	void UpdateView();
	/** The guide is up and lit: start playing, and take the command line's corner and camera. */
	void StartRunning();
	void LogState(const TCHAR* What) const;

	TArray<FApexGuideFileInfo> Files;
	bool bScanned = false;

	EApexGuideState State = EApexGuideState::Idle;
	FApexTrackGuide Guide;
	FString GuidePath;
	TSharedPtr<FApexReplayClip> Clip;
	/** The recording being read: the clip, or null and why. */
	struct FLoaded
	{
		TSharedPtr<FApexReplayClip> Clip;
		FString Error;
	};
	TFuture<FLoaded> Loading;
	float StateSeconds = 0.0f;
	float Reveal = 0.0f;

	ApexGuide::FPlayer Player;
	/** What the camera was last chosen for: the phase and the corner. */
	ApexGuide::EPhase ShownPhase = ApexGuide::EPhase::Overview;
	int32 ShownCorner = INDEX_NONE;
	bool bShownValid = false;

	TArray<FCamera> Cameras;
	int32 CameraIndex = 0;
	/** The player picked a car-relative camera: it stays across corners. */
	bool bKeepCarCamera = false;
	EApexGuideCameraKind KeptKind = EApexGuideCameraKind::Chase;
	/** `apexsim.guide.FrameBias` as the camera on screen was given it. */
	float AppliedFrameBias = -1.0f;

	/** -ApexGuide=: done once. */
	bool bCommandLineApplied = false;
	/** -ApexGuideCorner= / -ApexGuideCamera=, waiting for the guide to run. */
	int32 PendingCorner = 0;
	int32 PendingCamera = 0;
};
