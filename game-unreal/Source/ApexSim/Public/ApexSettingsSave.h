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

	/** Automatic shifting is done on the client, from the followed car's RPM. */
	UPROPERTY()
	bool bAutoGearbox = true;

	UPROPERTY()
	EApexRacingLine RacingLine = EApexRacingLine::BrakingOnly;

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

	/** Horizontal field of view of the driving cameras, in degrees. */
	UPROPERTY()
	float FieldOfView = 96.0f;

	// --- Controls -------------------------------------------------------------

	/** 0..1. Curves the steering axis: 0.5 is linear, below that is gentler. */
	UPROPERTY()
	float SteeringSensitivity = 0.62f;

	/** 0..1 of the axis range ignored around centre. */
	UPROPERTY()
	float Deadzone = 0.08f;

	/** 0..1. Stored and reported; nothing consumes it until force feedback lands. */
	UPROPERTY()
	float Vibration = 0.45f;

	/** Empty until something is rebound — an absent slot uses its default key. */
	UPROPERTY()
	TArray<FApexKeyBinding> Bindings;
};
