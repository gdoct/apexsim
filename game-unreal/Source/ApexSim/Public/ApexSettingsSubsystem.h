#pragma once

#include "CoreMinimal.h"
#include "ApexMultiView.h"
#include "ApexSettingsSave.h"
#include "ApexWheelProfiles.h"
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
	/** The driving aids the server runs for this car, and the racing line. */
	Assists,
	Graphics,
	Camera,
	Controls,
	/** The wheel: its devices, its forces and its own bindings. */
	Wheel,
	Audio,
	/** The garage: the car setup the server simulates this player with. */
	CarSetup,
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
	void SetSteeringAssist(bool bAssist);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetRacingLine(EApexRacingLine Line);

	/** The hotlap ghost car on or off (Gameplay group). */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetGhostCar(bool bOn);

	/** Whether a hotlap goes out on cold tyres (Gameplay group). */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetHotlapColdTyres(bool bCold);

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

	/** Modes the display actually supports, widest first. Cached after the first call. */
	const TArray<FIntPoint>& GetAvailableResolutions() const;

	// --- Screens --------------------------------------------------------------
	//
	// A triple-monitor rig: UApexMultiViewSubsystem applies these with the
	// rest of the graphics group. Screens goes to settings.yml as well.

	/** 1 or 3; anything else is 1. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetScreens(int32 Screens);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetTripleScreenWidth(float Cm);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetTripleBezel(float Cm);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetTripleEyeDistance(float Cm);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetTripleSideAngle(float Degrees);

	/** The rig as set up, in its ranges; the aspect is left at the default for the caller to fill in. */
	ApexMultiView::FTripleGeometry GetTripleGeometry() const;

	// --- Camera ---------------------------------------------------------------

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetFieldOfView(float Degrees);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetStartInCockpit(bool bCockpit);

	/** Remembers the chase distance the player last stepped to with C. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetChaseLevel(int32 Level);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetSeatForward(float Cm);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetSeatHeight(float Cm);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetViewPitch(float Degrees);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetHorizonLock(float Value01);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetHeadMotion(float Value01);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetLookToApex(float Value01);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetCockpitShowCar(bool bShow);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetCockpitWheel(bool bShow);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetCockpitMirrors(bool bShow);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetVirtualMirror(bool bShow);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetMirrorQuality(int32 Bucket);

	// --- Controls -------------------------------------------------------------

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetSteeringSensitivity(float Value01);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetDeadzone(float Value01);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetVibration(float Value01);

	/**
	 * The key on a slot, falling back to the slot's default when unbound.
	 *
	 * A wheel slot resolves to the attached device's binding, so a player with
	 * two wheelbases sees the one they are using (ApexInput::FindBinding).
	 */
	FKey GetBoundKey(FName ActionId, int32 Slot) const;

	/**
	 * An invalid key clears the slot without restoring its default. `bInvert`
	 * is for a device axis that runs backwards — a pedal resting at the top of
	 * its travel — and is worked out while the binding is captured.
	 */
	void SetBoundKey(FName ActionId, int32 Slot, const FKey& Key, bool bInvert = false);

	/** True when the slot's binding is on a device that is not attached now. */
	bool IsBindingDetached(FName ActionId, int32 Slot) const;

	/** Slots whose key equals this one, excluding the slot being edited. */
	TArray<const ApexInput::FSlotDef*> FindConflicts(const FKey& Key, FName ExceptAction, int32 ExceptSlot) const;

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void ResetBindings();

	/** True when the key matches any slot of the pause action, wheel buttons included. */
	bool IsPauseKey(const FKey& Key) const;

	// --- Wheel ----------------------------------------------------------------

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetWheelForce(float Value01);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetWheelRoadEffects(float Value01);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetWheelDamping(float Value01);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetWheelInvertForce(bool bInvert);

	/** The base's rotation lock to lock, as its own driver has it; clamped to what the Wheel page offers. */
	void SetWheelRotation(float Degrees);

	/** Degrees of rim, lock to lock, for the car's full steering lock; turns Auto off. */
	void SetWheelSteeringLock(float Degrees);

	/** Take the steering lock from the car being driven (UApexSettingsSave::bWheelSteeringLockAuto). */
	void SetWheelSteeringLockAuto(bool bAuto);

	/**
	 * The lock to lock of the car the player is driving, from its cockpit
	 * layout; 0 when there is none. Set every frame by the race director,
	 * not saved.
	 */
	void SetCarSteeringLock(float LockToLockDeg) { CarSteeringLockDeg = LockToLockDeg; }

	/**
	 * Rim degrees lock to lock that give full steering, as the gain applies
	 * it: the car's lock in Auto (the manual one until a car is known),
	 * never longer than the base's rotation.
	 */
	float GetWheelSteeringLockDeg() const;

	/** The wheel's axis to the steering: rotation over lock (ApexInput::WheelSteeringScale). */
	float GetWheelSteeringScale() const;

	/**
	 * The DirectInput device the forces go to — the one the steering is bound
	 * to — or INDEX_NONE when there is no wheel that can play them.
	 */
	int32 GetWheelDeviceSlot() const;

	/** The force-feedback wheel's profile, as the mixer uses it and the Wheel page shows it. */
	struct FWheelProfileState
	{
		/** A wheel that can play forces is attached; everything below is about it. */
		bool bHasBase = false;
		uint16 VendorId = 0;
		uint16 ProductId = 0;
		/** What its name matched (Generic when nothing did), and what is in use. */
		const ApexWheelProfiles::FProfile* Detected = &ApexWheelProfiles::Generic();
		const ApexWheelProfiles::FProfile* Profile = &ApexWheelProfiles::Generic();
		/** The player chose Profile rather than it being detected. */
		bool bChosen = false;
		/** Newton-metres in use, 0 when unknown, and whether the player gave it. */
		float PeakTorqueNm = 0.0f;
		bool bPeakGiven = false;
	};

	/**
	 * The profile for the wheel the forces go to: the player's choice for its
	 * model (UApexSettingsSave::WheelProfiles), else the one its name
	 * matches. Generic, with no base, when there is no such wheel.
	 */
	FWheelProfileState GetWheelProfile() const;

	/** Chooses the force wheel's profile by id; empty goes back to the detected one. */
	void SetWheelProfile(const FString& ProfileId);

	/** The force wheel's peak in newton-metres; 0 goes back to the profile's figure. */
	void SetWheelPeakTorque(float Nm);

	/**
	 * Whether the steering is bound to an attached wheel rather than a pad or
	 * the keyboard. The speed-sensitive steering aid is for sticks and keys:
	 * on a wheel it gears the rim down several times over at speed, which is
	 * what made the forces feel dead and a slide impossible to catch.
	 */
	bool IsSteeringOnWheel() const;

	/** Fires when a device is plugged in or pulled out, after the bindings are rebuilt. */
	FSimpleMulticastDelegate OnInputDevicesChanged;

	// --- Car setup ------------------------------------------------------------

	/** Moves one knob (ApexCarSetup::EKnob) to a click count, pinned to its range. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetCarSetupClick(int32 Knob, int32 Clicks);

	/** Replaces the whole working setup (a saved one being loaded). */
	void SetCarSetup(const FApexCarSetup& Setup);

	/** The saved setups made for one car, newest first. */
	TArray<FApexSavedSetup> GetSavedSetups(const FString& CarId) const;

	/** Saves the working setup under Name for CarId; returns the new entry's id. */
	FGuid SaveSetupAs(const FString& Name, const FString& CarId);

	/** Writes the working setup over a saved one (its best lap is forgotten). */
	void OverwriteSavedSetup(const FGuid& Id);

	void DeleteSavedSetup(const FGuid& Id);

	/** Gives a saved setup a new name; its clicks and best lap stay. */
	void RenameSavedSetup(const FGuid& Id, const FString& Name);

	/** Makes a saved setup the working one. */
	void LoadSavedSetup(const FGuid& Id);

	/** The saved setup last loaded or saved; invalid when the working setup came from none. */
	FGuid GetLoadedSetupId() const;

	/** Forgets which saved setup is loaded (a reset to stock). */
	void ClearLoadedSetup();

	/**
	 * A legal lap was driven on CarId: if the working setup is exactly the
	 * loaded saved one, it becomes that setup's best when quicker.
	 */
	void RecordSetupLap(const FString& CarId, int32 LapMs);

	// --- Audio ----------------------------------------------------------------

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetMasterVolume(float Value01);

	/** Read by UApexUiAudioSubsystem at play time; nothing to apply here. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetUiVolume(float Value01);

	/** Every car's engine; the race director puts it on the cars. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetEngineVolume(float Value01);

	/** Everybody else's engine against the player's own. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetOtherCarsVolume(float Value01);

	/** The driven car's tyres, kerbs, road and wind. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Settings")
	void SetRoadVolume(float Value01);

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
	void ApplyCamera();
	void ApplyGameplay();
	/** The aids reach the server through the root widget; this applies what is local (the line). */
	void ApplyAssists();
	void ApplyControls();
	void ApplyAudio();

	/**
	 * Registers the wheel's Menu* bindings with Slate's navigation config
	 * (ApexInput::ApplyMenuNavigation), so every widget that navigates by the
	 * config - ours, and Slate's sliders and dropdowns - takes them.
	 */
	void ApplyMenuNavigation();

	/** Undoes ApplyMenuNavigation: drops the rules it added. */
	void ClearMenuNavigation();

	/** The keys ApplyMenuNavigation added to the config, to take back out. */
	TArray<FKey> MenuNavigationKeys;

	/** SetCarSteeringLock's figure: the driven car's lock to lock, 0 for none. */
	float CarSteeringLockDeg = 0.0f;

	/** A device arrived or left: the bindings are rebuilt around what is here now. */
	void HandleInputDevicesChanged();

	/** Marks the preset Custom when a quality row no longer matches it. */
	void ReconcilePreset();

	UApexBootSettingsSubsystem* GetBoot() const;

	/** Mirrors the display block into settings.yml, which the player can edit. */
	void PushToBootSettings();

	UPROPERTY(Transient)
	TObjectPtr<UApexSettingsSave> Settings;

	mutable TArray<FIntPoint> AvailableResolutions;

	/** The input module's device-change subscription, dropped on the way out. */
	FDelegateHandle DevicesChangedHandle;

	int32 ChangeCount = 0;
	/** Suppresses the per-row counting and broadcasting while a preset applies. */
	bool bApplyingPreset = false;
};
