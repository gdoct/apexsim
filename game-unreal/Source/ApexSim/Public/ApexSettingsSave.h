#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "InputCoreTypes.h"

#include "ApexSettingsSave.generated.h"

/** How much of the race HUD is drawn. */
UENUM(BlueprintType)
enum class EApexHudDetail : uint8
{
	/** Everything, including the minimap and the pedal telemetry. */
	All,
	/** Race state, standings and the car's own numbers — nothing optional. */
	Essential,
	/** Nothing at all. */
	Hidden,
};

/** Three-step driving aid. */
UENUM(BlueprintType)
enum class EApexAssistLevel : uint8
{
	Off,
	Low,
	High,
};

UENUM(BlueprintType)
enum class EApexRacingLine : uint8
{
	Off,
	BrakingOnly,
	Full,
};

UENUM(BlueprintType)
enum class EApexUnits : uint8
{
	Metric,
	Imperial,
};

/** Scalability preset. Custom is what any hand-edited quality row falls back to. */
UENUM(BlueprintType)
enum class EApexGraphicsPreset : uint8
{
	Low,
	Medium,
	High,
	Ultra,
	Custom,
};

/**
 * One key bound to one action.
 *
 * An action can hold several of these — steering has a gamepad axis and two
 * keyboard halves — so the slot index, not the action id, is what a rebind
 * addresses. Slots are stable: the settings screen's row list names the slot it
 * edits, and the input config maps whatever key is stored there.
 */
USTRUCT()
struct APEXSIM_API FApexKeyBinding
{
	GENERATED_BODY()

	UPROPERTY()
	FName ActionId;

	UPROPERTY()
	int32 Slot = 0;

	UPROPERTY()
	FKey Key;

	/**
	 * Feeds the axis in the negative direction. Only meaningful for the
	 * keyboard halves of an Axis1D action — "steer left" is the same action as
	 * "steer right", inverted.
	 */
	UPROPERTY()
	bool bNegate = false;

	FApexKeyBinding() = default;
	FApexKeyBinding(FName InActionId, int32 InSlot, const FKey& InKey, bool bInNegate = false)
		: ActionId(InActionId), Slot(InSlot), Key(InKey), bNegate(bInNegate)
	{
	}
};

/**
 * Everything the settings overlay can change, in one save slot.
 *
 * Deliberately separate from UApexProfileSave: that slot is about what the
 * player picked (car, track, best laps), this one about how the game runs.
 * They have different lifetimes — wiping preferences should not lose a lap
 * record — and the settings overlay writes on every change, which is not
 * something the profile slot should be doing.
 *
 * Written through UApexSettingsSubsystem; nothing else should touch the slot.
 */
UCLASS()
class APEXSIM_API UApexSettingsSave : public USaveGame
{
	GENERATED_BODY()

public:
	static constexpr const TCHAR* SlotName = TEXT("ApexSettings");

	// --- Gameplay -------------------------------------------------------------

	UPROPERTY()
	EApexAssistLevel TractionControl = EApexAssistLevel::Low;

	UPROPERTY()
	bool bAbs = true;

	/** Automatic shifting, done by the server from the car's torque curve (SetDriverAids). */
	UPROPERTY()
	bool bAutoGearbox = true;

	/**
	 * Speed-sensitive steering, done by the server (SetDriverAids): full input
	 * asks for the tightest turn the car can hold at its speed, so the lock
	 * shrinks as speed rises. On by default: on a pad, the rack's full lock at
	 * racing speed is a spin a few millimetres of stick away.
	 */
	UPROPERTY()
	bool bSteeringAssist = true;

	/** The dotted line on the road: green flat out, amber at the limit, red braking. */
	UPROPERTY()
	EApexRacingLine RacingLine = EApexRacingLine::Off;

	/** 0..1. Applied to AI cars when a session is created. */
	UPROPERTY()
	float AiSkill = 0.74f;

	UPROPERTY()
	EApexUnits Units = EApexUnits::Metric;

	UPROPERTY()
	EApexHudDetail HudDetail = EApexHudDetail::All;

	// --- Graphics -------------------------------------------------------------

	UPROPERTY()
	EApexGraphicsPreset Preset = EApexGraphicsPreset::High;

	/** EWindowMode::Type as an int, to keep the engine enum out of the slot. */
	UPROPERTY()
	int32 DisplayMode = 0;

	UPROPERTY()
	FIntPoint Resolution = FIntPoint(1920, 1080);

	/** 0 means uncapped. */
	UPROPERTY()
	int32 FrameLimit = 144;

	UPROPERTY()
	bool bVSync = false;

	/** Scalability buckets, 0 (low) .. 3 (epic/ultra). */
	UPROPERTY()
	int32 ShadowQuality = 2;

	UPROPERTY()
	int32 AntiAliasingQuality = 2;

	UPROPERTY()
	int32 TextureQuality = 3;

	/** 0..1, driving r.MotionBlurQuality and the post-process amount. */
	UPROPERTY()
	float MotionBlur = 0.3f;

	// --- Camera ---------------------------------------------------------------
	//
	// The driver's view: where the seat is, how the head behaves, and what of
	// the cockpit is drawn. All of it previews live behind the settings panel.

	/** Horizontal field of view of the cockpit camera, in degrees; the chase view sits 15° narrower. */
	UPROPERTY()
	float FieldOfView = 96.0f;

	/** Which camera a race starts in. C swaps at any time. */
	UPROPERTY()
	bool bStartInCockpit = true;

	/** Seat slide from the car's own driving position, cm, positive forward. */
	UPROPERTY()
	float SeatForwardCm = 0.0f;

	/** Seat height from the car's own driving position, cm, positive up. */
	UPROPERTY()
	float SeatHeightCm = 0.0f;

	/** Resting gaze, degrees, positive looking up. */
	UPROPERTY()
	float ViewPitchDeg = 0.0f;

	/** 0..1. 0 rides the car's pitch and roll, 1 keeps the horizon level. */
	UPROPERTY()
	float HorizonLock = 0.25f;

	/** 0..1. How far braking and cornering throw the head. */
	UPROPERTY()
	float HeadMotion = 0.5f;

	/** 0..1. How far the head turns into a corner with the steering. */
	UPROPERTY()
	float LookToApex = 0.3f;

	/** Draw the car's own bodywork from inside; off for a mesh with no interior. */
	UPROPERTY()
	bool bCockpitShowCar = true;

	/** The steering wheel and its display. */
	UPROPERTY()
	bool bCockpitWheel = true;

	/** The mirrors on the car. */
	UPROPERTY()
	bool bCockpitMirrors = true;

	/** A rear-view strip at the top of the HUD, in either camera. */
	UPROPERTY()
	bool bVirtualMirror = false;

	/** 0 low .. 2 high: mirror capture resolution and refresh rate. */
	UPROPERTY()
	int32 MirrorQuality = 1;

	// --- Controls -------------------------------------------------------------

	/** 0..1. Curves the steering axis: 0.5 is linear, below that is gentler. */
	UPROPERTY()
	float SteeringSensitivity = 0.62f;

	/** 0..1 of the axis range ignored around centre. */
	UPROPERTY()
	float Deadzone = 0.08f;

	/**
	 * 0..1, force-feedback strength (ApexFfb::GainFromStrength): 0.5 plays the
	 * effects as designed, 0 is off. Read every frame, so a change is live.
	 */
	UPROPERTY()
	float Vibration = 0.45f;

	// --- Audio ----------------------------------------------------------------

	/** 0..1. Scales everything the game plays, through the audio device's primary volume. */
	UPROPERTY()
	float MasterVolume = 1.0f;

	/** 0..1. The menu's cues — moves, accepts, toasts — on top of the master volume. */
	UPROPERTY()
	float UiVolume = 0.8f;

	/** Empty until something is rebound — an absent slot uses its default key. */
	UPROPERTY()
	TArray<FApexKeyBinding> Bindings;
};
