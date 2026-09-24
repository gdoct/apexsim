#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "ApexProtocolTypes.h"
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
 * An action can hold several of these — steering has a gamepad axis, two
 * keyboard halves and a wheel's axis — so the slot index, not the action id, is
 * what a rebind addresses. Slots are stable: the settings screen's row list
 * names the slot it edits, and the input config maps whatever key is stored
 * there.
 *
 * The wheel column is the exception to one binding per slot: it holds one per
 * DirectInput device, so a player with two wheelbases keeps both mappings
 * (ApexInput::FindBinding).
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
	 * halves of an Axis1D action — "steer left" is the same action as
	 * "steer right", inverted. A property of the slot, not of the key.
	 */
	UPROPERTY()
	bool bNegate = false;

	/**
	 * The device's axis runs the other way: a pedal that rests at the top of
	 * its range, a wheel wired backwards. Worked out while the binding is
	 * captured, from which way the axis moved, so nothing has to be inverted
	 * by hand.
	 */
	UPROPERTY()
	bool bInvert = false;

	FApexKeyBinding() = default;
	FApexKeyBinding(FName InActionId, int32 InSlot, const FKey& InKey, bool bInNegate = false, bool bInInvert = false)
		: ActionId(InActionId), Slot(InSlot), Key(InKey), bNegate(bInNegate), bInvert(bInInvert)
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

	/** In a hotlap, the record lap's ghost car drives alongside. Toggled from the garage. */
	UPROPERTY()
	bool bGhostCar = true;

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

	/** Which camera a race starts in. C steps through them at any time. */
	UPROPERTY()
	bool bStartInCockpit = true;

	/**
	 * Which rung of the chase ladder the chase camera sits on: an index into
	 * `ApexChase::Views()`, closest first. 3 is the farthest, which is where
	 * the one chase camera used to be, so an existing profile is unchanged.
	 */
	UPROPERTY()
	int32 ChaseViewLevel = 3;

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
	 * 0..1, pad rumble strength (ApexFfb::GainFromStrength): 0.5 plays the
	 * effects as designed, 0 is off. Read every frame, so a change is live.
	 */
	UPROPERTY()
	float Vibration = 0.45f;

	// --- Wheel ----------------------------------------------------------------
	//
	// A wheelbase's forces, as fractions of whatever the device can produce:
	// the same numbers have to suit a 2 Nm belt drive and a 25 Nm direct drive,
	// so the base's own strength setting still does the coarse work.

	/**
	 * 0..1: how hard the steering torque pushes. 0.5 puts a car at the front
	 * axle's grip limit at about 60% of the base's peak, which leaves room for
	 * the extra load downforce brings. 0 is off.
	 */
	UPROPERTY()
	float WheelForce = 0.5f;

	/** 0..1: curbs, grass, ABS and impacts on top of the torque. 0.5 is as designed. */
	UPROPERTY()
	float WheelRoadEffects = 0.5f;

	/**
	 * 0..1: resistance to turning the rim, strongest at a standstill. A little
	 * is worth having — the torque arrives over the network, and a delayed
	 * force with nothing damping it is what makes a wheel oscillate.
	 */
	UPROPERTY()
	float WheelDamping = 0.25f;

	/**
	 * The base pulls the wrong way. Which direction a positive force turns the
	 * rim is the device's business, not something DirectInput promises, so this
	 * is a switch rather than a guess (the Wheel page has a test that moves it).
	 */
	UPROPERTY()
	bool bWheelInvertForce = false;

	/**
	 * Degrees the wheelbase turns lock to lock, as set in its own driver
	 * (Fanatec's SEN, Logitech's operating range). DirectInput reports only
	 * where the rim is between its two ends, not how far apart they are, so
	 * the player says; it is what turns the steering lock below into a scale.
	 */
	UPROPERTY()
	float WheelRotationDeg = 900.0f;

	/**
	 * Degrees of rim, lock to lock, that turn the car's front wheels to full
	 * lock. Shorter than the base's rotation gears the steering up, as a race
	 * car's rack is, and past it the rim meets a soft stop. Before this a
	 * 1080-degree base needed 540 degrees of rim for full lock.
	 */
	UPROPERTY()
	float WheelSteeringLockDeg = 480.0f;

	// --- Audio ----------------------------------------------------------------

	/** 0..1. Scales everything the game plays, through the audio device's primary volume. */
	UPROPERTY()
	float MasterVolume = 1.0f;

	/** 0..1. The menu's cues — moves, accepts, toasts — on top of the master volume. */
	UPROPERTY()
	float UiVolume = 0.8f;

	/** 0..1. Every car's engine (AApexRaceCarActor::SetMixVolumes), on top of the master volume. */
	UPROPERTY()
	float EngineVolume = 1.0f;

	/**
	 * 0..1. Everybody else's engine against the player's own, on top of
	 * EngineVolume. Half by default: the distance falloff already keeps a car
	 * up the road quiet, this is for the one alongside.
	 */
	UPROPERTY()
	float OtherCarsVolume = 0.5f;

	/** 0..1. The driven car's tyres, kerbs, road and wind. */
	UPROPERTY()
	float RoadVolume = 0.8f;

	/** Empty until something is rebound — an absent slot uses its default key. */
	UPROPERTY()
	TArray<FApexKeyBinding> Bindings;

	// --- Car setup ------------------------------------------------------------

	/**
	 * The garage setup, as clicks per knob (ApexCarSetup::EKnob), sent to the
	 * server as SetCarSetup on join and on every change. One setup for every
	 * car for now: the clicks are relative to whichever car.toml is driven.
	 */
	UPROPERTY()
	FApexCarSetup CarSetup;
};
