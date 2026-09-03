#pragma once

#include "CoreMinimal.h"
#include "ApexSettingsSave.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "ApexSettingsSubsystem.generated.h"

class UApexBootSettingsSubsystem;
class UApexInputConfig;

namespace ApexInput { struct FSlotDef; }

/** Which group changed, so a listener only refreshes what it cares about. */
UENUM(BlueprintType)
enum class EApexSettingsGroup : uint8
{
	Gameplay,
	Graphics,
	Controls,
	Audio,
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FApexOnSettingsChanged, EApexSettingsGroup, Group);

/**
 * Owns the settings slot, applies it, and tells everyone when it moves.
 *
 * The settings overlay does not hold state of its own: it reads through here
 * and writes back through here, so a change made while paused reaches the HUD,
 * the cameras and the input config without any of them knowing the overlay
 * exists. "Applies immediately, saved on close" in the design is exactly that —
 * every setter applies, and the slot is flushed when the overlay closes.
 *
 * Lives on the game instance because graphics and bindings outlive any one
 * session, and the race view has no widget tree of its own to hang them off.
 */
UCLASS()
class APEXSIM_API UApexSettingsSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	UPROPERTY(BlueprintAssignable, Category = "ApexSim|Settings")
	FApexOnSettingsChanged OnSettingsChanged;

	/** The live values. Read freely; write through the setters so they apply. */
	UFUNCTION(BlueprintPure, Category = "ApexSim|Settings")
	UApexSettingsSave* Get() const { return Settings; }

	// --- Gameplay -------------------------------------------------------------

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetTractionControl(EApexAssistLevel Level);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetAbs(bool bEnabled);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetAutoGearbox(bool bAuto);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetRacingLine(EApexRacingLine Line);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetAiSkill(float Skill01);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetUnits(EApexUnits InUnits);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetHudDetail(EApexHudDetail Detail);

	// --- Graphics -------------------------------------------------------------

	/** Moves every quality row to the preset's bucket. Custom leaves them alone. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetGraphicsPreset(EApexGraphicsPreset Preset);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetDisplayMode(int32 WindowMode);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetResolution(FIntPoint Resolution);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetFrameLimit(int32 Fps);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetVSync(bool bEnabled);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetShadowQuality(int32 Bucket);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetAntiAliasingQuality(int32 Bucket);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetTextureQuality(int32 Bucket);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetMotionBlur(float Amount01);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetFieldOfView(float Degrees);

	/** Modes the display actually supports, widest first. Cached after the first call. */
	const TArray<FIntPoint>& GetAvailableResolutions() const;

	// --- Controls -------------------------------------------------------------

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetSteeringSensitivity(float Value01);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetDeadzone(float Value01);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetVibration(float Value01);

	/** The key on a slot, falling back to the slot's default when unbound. */
	FKey GetBoundKey(FName ActionId, int32 Slot) const;

	/** An invalid key clears the slot without restoring its default. */
	void SetBoundKey(FName ActionId, int32 Slot, const FKey& Key);

	/** Slots whose key equals this one, excluding the slot being edited. */
	TArray<const ApexInput::FSlotDef*> FindConflicts(const FKey& Key, FName ExceptAction, int32 ExceptSlot) const;

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void ResetBindings();

	/** True when the key matches either slot of the pause action. */
	bool IsPauseKey(const FKey& Key) const;

	// --- Audio ----------------------------------------------------------------

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetMasterVolume(float Value01);

	/** Read by UApexUiAudioSubsystem at play time; nothing to apply here. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetUiVolume(float Value01);

	/**
	 * Applies the shaped steering curve — deadzone, then sensitivity — to a raw
	 * axis reading. Called on the input the controller collects, so a wheel and
	 * a keyboard both go through the same shaping.
	 */
	float ShapeSteering(float RawAxis) const;

	// --- Persistence ----------------------------------------------------------

	/** Writes the slot. Cheap enough to call on close; not on every drag frame. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void Save();

	/** Restores one group to its shipped values and applies it. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void ResetToDefaults(EApexSettingsGroup Group);

	/** Changes made since the overlay was last opened; shown in its footer. */
	UFUNCTION(BlueprintPure, Category = "ApexSim|Settings")
	int32 GetChangeCount() const { return ChangeCount; }

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void ResetChangeCount() { ChangeCount = 0; }

	/** Pushes the whole of a group at whatever consumes it. */
	void ApplyGroup(EApexSettingsGroup Group);

private:
	void Load();
	/** Applies, counts the change, and broadcasts. Every setter ends here. */
	void Changed(EApexSettingsGroup Group);

	void ApplyGraphics();
	void ApplyGameplay();
	void ApplyControls();
	void ApplyAudio();

	/** Marks the preset Custom when a quality row no longer matches it. */
	void ReconcilePreset();

	UApexBootSettingsSubsystem* GetBoot() const;

	/** Mirrors the display block into settings.yml, which the player can edit. */
	void PushToBootSettings();

	UPROPERTY(Transient)
	TObjectPtr<UApexSettingsSave> Settings;

	mutable TArray<FIntPoint> AvailableResolutions;

	int32 ChangeCount = 0;
	/** Suppresses the per-row counting and broadcasting while a preset applies. */
	bool bApplyingPreset = false;
};
