#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "GameFramework/Actor.h"
#include "Race/ApexChaseView.h"
#include "Race/ApexReplayCamera.h"
#include "Race/ApexSkyModel.h"
#include "Race/ApexTvDirector.h"

#include "ApexRaceDirector.generated.h"

class AApexCockpitRig;
class FApexReplayClip;
class AApexGhostCarActor;
class AApexRaceCarActor;
class AApexRacingLineActor;
class AApexRainActor;
class ADirectionalLight;
class APostProcessVolume;
class ASkyLight;
class UApexMenuFlowSubsystem;
class UApexNetSubsystem;
class UApexSettingsSubsystem;
class UCameraComponent;
class ULevelStreamingDynamic;
class UMaterialInstanceDynamic;
class USpringArmComponent;
class UTextureRenderTarget2D;

/**
 * Spawns a car per roster entry and drives them from telemetry.
 *
 * Cars are driven purely from telemetry: the client runs no physics and no
 * collision of its own, it just places each car where the server says it is.
 *
 * The circuit itself is streamed in alongside them, from the level the
 * `ApexTrackImport` commandlet generated for the session's track. It is a
 * level *instance* rather than a map travel on purpose — travelling would
 * tear down the menu world this director and the whole UI live in.
 *
 * Placed once in the menu level. It does nothing until a session is joined and
 * the UDP handshake completes.
 */
UCLASS()
class APEXSIM_API AApexRaceDirector : public AActor
{
	GENERATED_BODY()

public:
	AApexRaceDirector();

	virtual void Tick(float DeltaSeconds) override;

	/** Finds the director in the current world, or null. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Race", meta = (WorldContext = "WorldContextObject"))
	static AApexRaceDirector* Find(const UObject* WorldContextObject);

	/** Takes over the view: possesses the camera and starts following the local car. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Race")
	void BeginRaceView();

	/** Hands the view back and despawns the cars. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Race")
	void EndRaceView();

	UFUNCTION(BlueprintPure, Category = "ApexSim|Race")
	bool IsRaceViewActive() const { return bRaceViewActive; }

	/** Swap between the cockpit view and the chase camera. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Race")
	void SetCockpitView(bool bCockpit);

	UFUNCTION(BlueprintPure, Category = "ApexSim|Race")
	bool IsCockpitView() const { return bCockpitView; }

	/**
	 * Which rung of the chase ladder the chase camera sits on (ApexChase).
	 * Setting one leaves the cockpit if that is where the view was, so a
	 * console command or a setting can pick a distance outright.
	 */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Race")
	void SetChaseLevel(int32 Level);

	UFUNCTION(BlueprintPure, Category = "ApexSim|Race")
	int32 GetChaseLevel() const { return ChaseLevel; }

	/** What C does: cockpit, then each chase distance in turn, then back. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Race")
	void CycleView();

	/**
	 * The broadcast camera (ApexTvDirector) instead of the driving cameras:
	 * always on in the demo view, `-ApexView=tv` or `apexsim.tv.View 1` in a race.
	 */
	void SetTvView(bool bTv);
	bool IsTvView() const { return bTvView; }

	/** The broadcast camera's director, for the console commands and the debug readout. */
	ApexTv::FDirector& GetTvDirector() { return Tv; }

	// --- Demo view ---------------------------------------------------------------

	/**
	 * Show the menu's demo race behind the shell: stream the circuit in, spawn
	 * the AI field from the roster as it arrives, and film it with the
	 * broadcast camera. No HUD, no cockpit, no input: the menu keeps all of
	 * that. A race view that begins takes over from it.
	 *
	 * @param TrackStem   the track's YAML base name, which names its level
	 * @param Centerline  the track's centerline from the lobby (server frame),
	 *                    where the trackside cameras stand
	 */
	void BeginDemoView(const FString& TrackStem, const TArray<FVector2D>& Centerline);

	/** Drop the demo race: cars, circuit and lighting go back to the menu's. */
	void EndDemoView();

	bool IsDemoViewActive() const { return bDemoView; }

	/**
	 * Hide the demo's world without ending it, for a screen that renders its
	 * own 3D preview in the same world (the car picker's turntable would pick
	 * up the circuit and the race lighting).
	 */
	void SetDemoWorldVisible(bool bVisible);

	/**
	 * How much of the demo the menu should let through, 0..1: rises once the
	 * circuit is in and the camera has a shot, falls when the world is hidden.
	 * The shell fades its backdrop by it, so the menu never shows a half-loaded
	 * level or the empty menu world.
	 */
	float GetDemoBackdropOpacity() const { return DemoOpacity; }

	/**
	 * True once the demo is all the way in: faded up to full opacity, or loaded
	 * with cars on the road behind a screen that keeps the world hidden. The
	 * startup splash holds until this, so the first frame the player sees is
	 * the menu over a running race.
	 */
	bool IsDemoReady() const;

	/** Fade the backdrop out ahead of ending the demo; watch GetDemoBackdropOpacity reach 0. */
	void FadeOutDemo() { bDemoFadeOut = true; }

	// --- Replay clip (offline playback) ------------------------------------------------

	/**
	 * Play a race clip from disk (`-ApexReplay=`, UApexReplaySubsystem): no
	 * server, no session. The clip's track streams in by its stem, the field
	 * spawns from its roster, the sky is `Conditions`, and the cars are
	 * placed straight from the clip at the director's own clock (game time,
	 * so a fixed-timestep frame dump is exact) and filmed as `Camera` says.
	 * The clock holds the first frame until PlayReplay.
	 */
	void BeginReplayView(TSharedPtr<const FApexReplayClip> Clip, const ApexReplayCam::FSettings& Camera,
		const FApexSessionConditions& Conditions);
	void EndReplayView();
	bool IsReplayViewActive() const { return bReplayView; }

	/**
	 * Everything is in to film: the level streamed and visible with its sky
	 * applied, and the cars placed. A clip whose track has no imported level
	 * never gets here; the caller times out.
	 */
	bool IsReplayReady() const;
	/** True when the clip's track has an imported level being streamed. */
	bool HasReplayTrackLevel() const { return bReplayView && TrackLevel != nullptr; }

	/** Start the clip's clock at `FromSeconds` (from its first frame). */
	void PlayReplay(double FromSeconds);
	bool IsReplayPlaying() const { return bReplayPlaying; }
	/** Seconds from the clip's first frame that the cars are showing. */
	double GetReplayTime() const { return ReplayTime; }

	/** Horizontal field of view of both driving cameras, in degrees. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Race")
	void SetFieldOfView(float Degrees);

	/**
	 * Re-read the camera block of the settings: field of view, seat, head
	 * behaviour and what of the cockpit is drawn. Called by the settings
	 * subsystem on every change and by BeginRaceView, so a slider dragged
	 * behind the pause panel moves the seat as it goes.
	 */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Race")
	void ApplyCameraSettings();

	/**
	 * Re-read the audio block of the settings onto every car: the engine and
	 * the tyres/road volumes. Called by the settings subsystem on a change and
	 * whenever a car is spawned.
	 */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Race")
	void ApplyAudioSettings();

	/**
	 * Show or hide the racing line as the Gameplay settings ask. Called by the
	 * settings subsystem on every change and by BeginRaceView;
	 * `-ApexRacingLine=off|braking|full` overrides it for a screenshot run.
	 */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Race")
	void ApplyRacingLineSetting();

	/**
	 * Park the free screenshot camera at a pose in Unreal world space and
	 * make it the view. It stays there — cars spawning, the followed car
	 * changing and C all leave it alone — until `ReleaseShotCamera`. Set
	 * outside a race, it is waiting for the next race view.
	 */
	void SetShotCameraPose(const FVector& LocationCm, const FRotator& Rotation);

	/** Back to the cockpit or chase camera. */
	void ReleaseShotCamera();

	bool HasShotCameraPose() const { return bShotCameraPose; }

	/** Horizontal field of view of the shot camera, in degrees. */
	void SetShotCameraFov(float Degrees);

	/** The rear view the HUD's virtual mirror shows, or null while it is off. */
	UTextureRenderTarget2D* GetVirtualMirrorTexture() const;

	/** The car the camera is following, or null outside a race. */
	AApexRaceCarActor* GetFollowedCar() const { return FollowedCar; }

	// --- Ghost and replay (hotlap) ---------------------------------------------------

	/**
	 * Play the local driver's record lap back: the ghost drives it from the
	 * line in real time while the camera cuts between a chase shot, a
	 * trackside camera and the onboard view, and the view returns to the
	 * player's car when the lap is over (or on EndGhostReplay). False when
	 * there is no record lap to play.
	 */
	bool BeginGhostReplay();
	void EndGhostReplay();
	bool IsGhostReplayActive() const { return bGhostReplay; }
	/** Milliseconds into the replayed lap; negative during the lead-in at the line. */
	float GetGhostReplayTimeMs() const { return ReplayClockMs; }
	/** The lap being replayed, ms; 0 when none. */
	int32 GetGhostLapTimeMs() const;

	/** Speed of the car the camera is following, in km/h. */
	UFUNCTION(BlueprintPure, Category = "ApexSim|Race")
	float GetFollowedSpeedKph() const;

	UFUNCTION(BlueprintPure, Category = "ApexSim|Race")
	int32 GetSpawnedCarCount() const { return Cars.Num(); }

	/** True once the session's track level has finished streaming in. */
	UFUNCTION(BlueprintPure, Category = "ApexSim|Race")
	bool IsTrackLevelLoaded() const;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USceneComponent> Root;

	/** Chase camera. Follows the local car, or car 0 if the roster has no local entry. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USpringArmComponent> CameraBoom;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UCameraComponent> ChaseCamera;

	/**
	 * Driver's-eye camera. Not on the boom: the boom deliberately lags and
	 * ignores pitch and roll, which is right for a chase view and wrong for
	 * a cockpit, where the horizon tilting with the car is most of the point.
	 * Where it sits comes from the car's cockpit layout plus the seat settings.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UCameraComponent> CockpitCamera;

	/**
	 * Free camera for screenshots from anywhere on the circuit, placed by
	 * `apexsim.cam.*` or `-ApexCamera=` / `-ApexCameraLookAt=`. Absolute and
	 * untouched by the driving-camera updates; active only while a shot pose
	 * is set.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UCameraComponent> ShotCamera;

	/**
	 * The broadcast camera, placed every frame by the TV director. Absolute,
	 * like the shot camera; carries its own depth of field for the long lens.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UCameraComponent> TvCamera;

	/** Fallback mesh for cars with no catalog row (AI drivers have no car id). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|Race")
	TSoftObjectPtr<UStaticMesh> DefaultCarMesh;

private:
	UFUNCTION()
	void HandleRosterUpdated(const FApexSessionRoster& Roster);

	UFUNCTION()
	void HandleTelemetry(const FApexTelemetryFrame& Frame);

	UFUNCTION()
	void HandleSessionLeft();

	UFUNCTION()
	void HandleLobbyStateUpdated(const FApexLobbyState& LobbyState);

	UFUNCTION()
	void HandleRacingLineUpdated(const FApexRacingLineData& Line);

	/** Spawn the racing line for this race with whatever line has arrived, or tear it down. */
	void EnsureRacingLine();
	void DestroyRacingLine();
	/** Drop the dots onto the road once the track level's geometry is in the world. */
	void SnapRacingLineToTrack();

	/** Creates or destroys car actors so they match the roster. */
	void SyncCarsToRoster(const FApexSessionRoster& Roster);
	/** Gives a car the catalog's mesh, wheels and cockpit for `CarId`, or the fallback. */
	/** The car's mesh, wheels, cockpit and sound from its catalog row, in `Livery` (0: as authored). */
	void ApplyCatalogMesh(AApexRaceCarActor* Car, const FString& CarId, int32 Livery = 0);

	UFUNCTION()
	void HandleGhostLap(const FApexGhostLap& Lap);

	/** Spawn the ghost for the lap the net subsystem holds, or tear it down. */
	void EnsureGhost();
	void DestroyGhost();
	/**
	 * Drive the ghost round with the local car's lap clock in a hotlap:
	 * shown from the line on, hidden in the garage, on the run-up and while
	 * it overlaps the player's car.
	 */
	void UpdateGhost(float DeltaSeconds);
	/** Advance the replay's clock and cut its cameras. */
	void UpdateGhostReplay(float DeltaSeconds);
	/** Put the replay's camera on its next shot. */
	void CutReplayShot();
	AApexRaceCarActor* FindCar(int32 CarIndex) const;
	void DestroyAllCars();
	/** Points the camera boom at whichever car the local player is driving. */
	void UpdateCameraTarget();

	/** Placeholder keyboard driving so the transport milestone is testable. */
	void PollDrivingInput();

	/**
	 * The gear an automatic box would ask for this tick, or -128 for none.
	 *
	 * The protocol has no "automatic" flag — the server always takes the gear it
	 * is told — so the box is simply a client that sends the shift the driver
	 * would have sent.
	 */

	/** Camera-view and other non-driving keys. */
	void PollViewInput();

	/** Film the field: feed the TV director this frame's cars and place the camera where it says. */
	void UpdateTvCamera(float DeltaSeconds);

	/** Advance the replay clock and place every car from the clip. */
	void UpdateReplay(float DeltaSeconds);
	/** Put the car poses of one (interpolated) clip frame on the actors. */
	void ApplyReplayFrame(const FApexTelemetryFrame& Frame, float DeltaSeconds);
	/** The tripod and pan cameras of a replay; the other modes use the race's own. */
	void UpdateReplayCamera(float DeltaSeconds);
	/** Who a replay camera follows now, by the clip's follow rule. */
	int32 ResolveReplayFollow() const;

	/** Ease the demo backdrop's opacity toward whether there is anything worth showing. */
	void UpdateDemoOpacity(float DeltaSeconds);

	/** Cars, level and engine sound of the demo, shown or hidden together. */
	void ApplyDemoWorldVisibility();

	/**
	 * Give the player controller key input while racing, and hand it back to
	 * the menu afterwards.
	 *
	 * The menu shell runs in UI-only input, where the controller sees no key
	 * state at all and every driving key silently does nothing. Game-and-UI
	 * is what lets the driving keys through while Escape still reaches the
	 * root widget, which only handles Escape and lets everything else fall
	 * past it to the controller.
	 */
	void ApplyRaceInputMode(bool bRacing);

	/** Point the active camera and hide the car when sitting inside it. */
	void ApplyCameraMode();

	/** Put the boom where the current rung of the chase ladder says. */
	void ApplyChaseView();

	/** "cockpit", "chase close (3.0 m)", "broadcast": for the log. */
	FString DescribeView() const;

	/** Ride the followed car's orientation, as far as the horizon lock lets it. */
	void UpdateCockpitCamera();

	/**
	 * The driver's eye in the car's frame: the car's own seat, slid and
	 * raised by the settings, thrown about by inertia.
	 */
	FVector CockpitEyeLocal() const;

	/**
	 * Estimate the g the driver feels from the followed car's smoothed
	 * motion: speed change along the car, speed times yaw rate across it.
	 * The wire carries no acceleration, and none is needed for a head lean.
	 */
	void UpdateHeadMotion(float DeltaSeconds);

	/** Ease the head toward where the look keys, the stick and the steering say. */
	void UpdateLook(float DeltaSeconds);

	/** Spawn the cockpit rig for this race, or tear it down. */
	void EnsureRig();
	void DestroyRig();
	/** Tell the rig what to draw and capture, from the settings and the view. */
	void PushRigFeatures();

	/**
	 * Everything that makes the cameras feel speed: FOV that widens as the
	 * car gains pace, and a perlin micro-shake on both views. All of it is
	 * procedural — there are no camera-shake assets in the project — and all
	 * of it scales with speed squared, so a formation-lap crawl stays calm.
	 */
	void UpdateCameraFeel(float DeltaSeconds);

	/**
	 * Light the menu world for the session's sky (`ApexSky::Derive` over the
	 * session's conditions): aim its sun, or the moon, and set the light's
	 * strength and colour, the sky light's scale, the exposure clamp, the
	 * grading, the rain and the cars' headlights. Put the sky light on
	 * real-time capture so ambient light matches the atmosphere instead of a
	 * capture taken in the empty menu void. What lives in the streamed track
	 * level (fog, floodlights, the wet road) waits for
	 * ApplyTrackLevelConditions.
	 */
	void ApplyRaceEnvironment();
	/** Put the sun back the way the menu had it, and the rain and lights away. */
	void RestoreMenuEnvironment();
	/**
	 * The part of the sky that lives in the track level, applied once the
	 * level's actors are in the world: the fog's density and colour, the
	 * road's wet sheen, spot lights on the floodlight masts and the glow of
	 * their lamps.
	 */
	void ApplyTrackLevelConditions();
	/** Forget the level's lights and fog, ahead of a new level. */
	void ForgetTrackLevelConditions();
	/** The sky the race is lit for. */
	const ApexSky::FSkyState& GetSky() const { return Sky; }

	/**
	 * Content path of the level for the session's track, or empty if there
	 * is no session, no track, or no imported level for it.
	 *
	 * Resolved by convention from the track file the server names —
	 * `tracks/real/Monza.yaml` -> `/Game/Tracks/Monza/L_Monza` — which is
	 * exactly how the importer names what it generates, so the two cannot
	 * drift apart without the lookup failing loudly.
	 */
	FString ResolveTrackLevelPath() const;

	void LoadTrackLevel();
	void UnloadTrackLevel();

	/**
	 * Checks the content about to be shown against the server's files: the
	 * track as its level streams, the local player's car as it spawns. Each is
	 * checked once per race; a demo only logs, a real session also toasts.
	 */
	void VerifyTrackContent();
	void VerifyLocalCarContent();
	FString VerifiedTrackId;
	FString VerifiedCarId;

	/**
	 * Drive the start-light gantry the track importer places past the line
	 * from the countdown in the telemetry frame: one light per second over
	 * the last five seconds, all out when the session goes racing.
	 */
	void UpdateStartLights(const FApexTelemetryFrame& Frame);
	/**
	 * Beep the race start and the player's laps: a tick for each of the last
	 * five countdown seconds (with the lights), a higher tone on green, and a
	 * double pip each time the local car crosses the line into a new lap.
	 * Silent in the demo behind the menu.
	 */
	void UpdateRaceBleeps(const FApexTelemetryFrame& Frame, EApexSessionState PreviousState);
	/** Whole countdown seconds left at the last tick; -1 when not counting down. */
	int32 BleepCountdownSecond = -1;
	/** The local car's lap at the last frame and whose it was; -1 until seen. */
	int32 BleepLap = -1;
	int32 BleepCarIndex = -1;

	/** Find the gantry in the streamed level and take over its lens materials. */
	void FindStartLights();
	void ForgetStartLights();

	/** Lens materials of the five lights, left to right as seen from the grid. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> StartLightLenses;
	/** How many lenses are lit; -1 forces a refresh on the next frame. */
	int32 LitStartLights = -1;
	/** Done once per streamed level; a level without a gantry stops the search. */
	bool bSearchedStartLights = false;
	/** Emissive strength of a lit lens. The race is exposed for a 50 klux sun. */
	static constexpr float StartLightOnEmissive = 4000.0f;

	UPROPERTY(Transient)
	TObjectPtr<ULevelStreamingDynamic> TrackLevel;

	UApexNetSubsystem* GetNet() const;
	UApexMenuFlowSubsystem* GetFlow() const;
	UApexSettingsSubsystem* GetSettings() const;


	UPROPERTY(Transient)
	TMap<int32, TObjectPtr<AApexRaceCarActor>> Cars;

	/**
	 * The car id each index's mesh was chosen for. Indices follow the
	 * roster's order, so when someone leaves an actor can pass to a driver
	 * in a different car.
	 */
	TMap<int32, FString> CarIdShown;

	UPROPERTY(Transient)
	TObjectPtr<AApexRaceCarActor> FollowedCar;

	/** The record lap's puppet; exists only in a hotlap with a lap to drive. */
	UPROPERTY(Transient)
	TObjectPtr<AApexGhostCarActor> Ghost;
	/** The ghost's lap clock, eased toward the local car's lap time. */
	float GhostClockMs = -1.0f;
	bool bGhostClockValid = false;
	/** What the local car's telemetry last said, for the ghost and the replay. */
	bool bLocalInGarage = false;
	int32 LocalLap = 0;
	int32 LocalLapTimeMs = 0;
	/** Whether the server says the local car's headlights are on. */
	bool bLocalHeadlights = false;
	/**
	 * The player's headlight switch as sent (`FApexPlayerInput::Headlights`):
	 * -1 until the first press, leaving the lights to the sky; the first press
	 * flips whatever the server shows. Reset when a race view begins.
	 */
	int32 HeadlightSwitch = -1;
	/** The ghost is hidden this close to the player's car and shown again past the larger distance, cm. */
	static constexpr float GhostHideDistanceCm = 300.0f;
	static constexpr float GhostShowDistanceCm = 500.0f;
	bool bGhostOverlapHidden = false;

	bool bGhostReplay = false;
	float ReplayClockMs = 0.0f;
	/** The shot on screen and how long it has run; see CutReplayShot. */
	int32 ReplayShot = -1;
	float ReplayShotSeconds = 0.0f;
	FVector ReplayCameraLocation = FVector::ZeroVector;
	bool bReplayCockpitWas = true;
	/** The rung the player was on before a replay took the chase camera. */
	int32 ReplayChaseLevelWas = ApexChase::DefaultLevel();
	/** Seconds the ghost waits at the line before a replay rolls. */
	static constexpr float ReplayLeadInSeconds = 1.5f;
	/** The trackside camera stands this far up the road from the car, on this lens. */
	static constexpr float ReplayTracksideAheadCm = 16000.0f;
	static constexpr float ReplayTracksideFov = 38.0f;
	/** The shot camera's lens outside a trackside shot (its default). */
	static constexpr float ReplayShotCameraRestFov = 70.0f;

	/** Wheel, display and mirrors inside the followed car. Exists only while racing. */
	UPROPERTY(Transient)
	TObjectPtr<AApexCockpitRig> Rig;

	/** The dotted line on the road. Exists only while racing, hidden when the setting is off. */
	UPROPERTY(Transient)
	TObjectPtr<AApexRacingLineActor> RacingLine;

	/**
	 * Apply `-ApexCamera=X,Y,Z,Yaw,Pitch`, `-ApexCameraLookAt=X,Y,Z,TX,TY,TZ`
	 * and `-ApexCameraFov=` (server frame) for a screenshot run.
	 */
	void ApplyShotCameraCommandLine();

	bool bRaceViewActive = false;

	// --- Broadcast camera and demo --------------------------------------------------

	ApexTv::FDirector Tv;
	bool bTvView = false;
	/** The TV director has placed the camera at least once since the view began. */
	bool bHasTvPose = false;
	/** Cuts already written to the log (Verbose), so each is logged once. */
	int32 LoggedTvCuts = 0;
	bool bDemoView = false;

	/** A clip from disk is playing (BeginReplayView); the net is ignored. */
	bool bReplayView = false;
	bool bReplayPlaying = false;
	double ReplayTime = 0.0;
	TSharedPtr<const FApexReplayClip> ReplayClip;
	ApexReplayCam::FSettings ReplayCamera;
	FApexSessionConditions ReplayConditions;
	/** The car the replay's cameras follow; see ResolveReplayFollow. */
	int32 ReplayFollowIndex = INDEX_NONE;
	/** The pan camera's eased aim and lens. */
	FQuat ReplayPanRotation = FQuat::Identity;
	float ReplayPanFov = 50.0f;
	bool bReplayPanValid = false;
	/** The tripod has been checked against the level's ground (SeatReplayEye). */
	bool bReplayEyeSeated = false;
	/** Lift a buried tripod out of the rendered ground, once the level is visible. */
	void SeatReplayEye();

	bool bDemoWorldVisible = true;
	bool bDemoFadeOut = false;
	FString DemoTrackStem;
	float DemoOpacity = 0.0f;
	/** Seconds the demo has had everything it needs on screen. */
	float DemoReadyFor = 0.0f;
	/** The demo's cars sit under the menu's own sounds. */
	static constexpr float DemoEngineVolume = 0.35f;

	/**
	 * Tyres, kerbs, road and wind for the local car, from the server's
	 * DriverFeedback reduced to the force feedback's signals (ApexFfb), and
	 * which car hears its engine from inside a closed cabin. Every frame.
	 */
	void UpdateCarAudio();
	/** The feedback message last turned into sound: its hits play once. */
	uint32 LastRoadFeedbackSerial = 0;

	/** The newest frame's state; the net subsystem keeps a demo's state out of its own. */
	EApexSessionState LatestFrameState = EApexSessionState::Lobby;

	/** What race order needs from the wire, per car index. */
	struct FCarProgress
	{
		int32 Lap = 0;
		float StationM = 0.0f;
		bool bOnTrack = true;
	};
	TMap<int32, FCarProgress> CarProgress;

	/** Velocity measured from each car's eased motion, for the camera's lead. */
	struct FCarMotion
	{
		FVector PrevLocation = FVector::ZeroVector;
		FVector Velocity = FVector::ZeroVector;
		bool bValid = false;
	};
	TMap<int32, FCarMotion> CarMotion;
	/** A shot pose is set: the shot camera is the view. */
	bool bShotCameraPose = false;
	/** The settings pick the view a race opens in; C swaps at any time. */
	bool bCockpitView = true;
	/**
	 * The chase camera's distance, an index into `ApexChase::Views()`. Kept
	 * while the view is in the cockpit, so C comes back to the distance the
	 * player last chose rather than to the far end of the ladder.
	 */
	int32 ChaseLevel = ApexChase::DefaultLevel();
	/** -ApexView= named a rung: the settings do not get to move it. */
	bool bChaseLevelFromCommandLine = false;

	// --- Head -------------------------------------------------------------------

	/** Where the head is turned this frame, degrees in the car's frame. */
	float LookYawDeg = 0.0f;
	/** Inertia offset of the eye, cm in the car's frame, eased. */
	FVector HeadOffset = FVector::ZeroVector;
	float LateralG = 0.0f;
	float LongitudinalG = 0.0f;
	FVector PrevLocation = FVector::ZeroVector;
	float PrevSpeedMps = 0.0f;
	float PrevYawDeg = 0.0f;
	bool bHavePrevMotion = false;
	/** Logged once per race so the transport can be confirmed from the log alone. */
	bool bLoggedFirstTelemetry = false;

	/** How far the FOV widens at full speed, degrees (chase; cockpit gets 60%). */
	static constexpr float SpeedFovBoostDeg = 10.0f;
	/** Speed at which the FOV boost and shake saturate, km/h. */
	static constexpr float FeelTopSpeedKph = 300.0f;

	/** FOV the settings asked for, before the speed boost. */
	float BaseCockpitFov = 95.0f;
	float BaseChaseFov = 80.0f;
	float CurrentFovBoost = 0.0f;
	/** This frame's cockpit micro-shake, composed onto the car rotation. */
	FRotator CockpitShake = FRotator::ZeroRotator;

	/** The session's sky as last applied; see ApexSkyModel.h. */
	ApexSky::FSkyState Sky;
	/** Unbound, above the track level's own: exposure and grading for the sky. */
	UPROPERTY(Transient)
	TObjectPtr<APostProcessVolume> SkyPostProcess;
	UPROPERTY(Transient)
	TObjectPtr<AApexRainActor> Rain;
	/** Carries the spot lights placed on the level's floodlight masts. */
	UPROPERTY(Transient)
	TObjectPtr<AActor> FloodlightRig;
	/** Set once the streamed level's fog and lights have been set for this sky. */
	bool bTrackConditionsApplied = false;
	/** Spot lights on masts at most: a big circuit has more masts than the frame has budget. */
	static constexpr int32 MaxFloodlights = 96;

	/** Menu-world sun state, saved before the race re-aims it. */
	TWeakObjectPtr<ADirectionalLight> MenuSun;
	FRotator MenuSunRotation = FRotator::ZeroRotator;
	FLinearColor MenuSunColor = FLinearColor::White;
	float MenuSunIntensity = 10.0f;
	bool bMenuSunSaved = false;
};
