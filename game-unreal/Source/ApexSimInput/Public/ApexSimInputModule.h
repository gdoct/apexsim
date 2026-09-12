#pragma once

#include "CoreMinimal.h"
#include "ApexDirectInputTypes.h"
#include "IInputDeviceModule.h"

class FApexDirectInputDevice;

/**
 * Wheels, pedals, shifters and button boxes, through DirectInput.
 *
 * XInput is the engine's pad path and knows nothing else; every sim-racing
 * device on Windows speaks DirectInput instead, force feedback included. This
 * module is an input-device plugin like the engine's XInput one: the
 * platform application asks it for a device on its first poll, ticks that
 * device every frame, and forwards WM_DEVICECHANGE to it so a device plugged
 * in mid-session shows up without a restart.
 *
 * XInput devices are skipped (the engine already reads them, and their
 * DirectInput face merges the triggers into one axis), so an Xbox pad is
 * never seen twice.
 *
 * Game code talks to the module, never to the device: the device only exists
 * after the first poll, while the settings subsystem wants to subscribe to
 * device changes at game-instance startup.
 */
class APEXSIMINPUT_API FApexSimInputModule : public IInputDeviceModule
{
public:
	/** Null when the module is not loaded. */
	static FApexSimInputModule* Get();

	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	virtual TSharedPtr<IInputDevice> CreateInputDevice(const TSharedRef<FGenericApplicationMessageHandler>& InMessageHandler) override;

	/** Devices attached now, in slot order. */
	const TArray<ApexDirectInput::FDeviceInfo>& GetDevices() const;

	const ApexDirectInput::FDeviceInfo* FindDevice(int32 Slot) const;

	bool IsAttached(int32 Slot) const { return FindDevice(Slot) != nullptr; }

	/** What a slot was last called, attached or not; null for a slot never used. */
	const ApexDirectInput::FSlotRecord* FindRecord(int32 Slot) const;

	/** Moves every time the attached set changes. */
	uint32 GetDevicesSerial() const { return DevicesSerial; }

	/** Fires on the game thread after the attached set changed. */
	FSimpleMulticastDelegate& OnDevicesChanged() { return DevicesChanged; }

	/** Latest reading: an axis -1..1, a button or hat direction 0 or 1. 0 for anything not attached. */
	float GetControlValue(const ApexDirectInput::FControl& Control) const;

	/** "WHEEL X", "PEDALS RZ", "WHEEL2 B12": short enough for a binding chip. */
	FString GetShortName(const FKey& Key) const;

	/** "FANATEC CSL DD · X": for prompts and lists. */
	FString GetLongName(const FKey& Key) const;

	/**
	 * Plays effects on a device until the next call. Call every frame: a device
	 * that has not heard from the game for a moment lets go of every force, so
	 * a hitch or a crash of the caller never leaves a wheel pulling to one side.
	 * INDEX_NONE releases whatever was playing.
	 */
	void SetWheelEffects(int32 Slot, const FApexWheelEffects& Effects);

	/** Enumerate again shortly; also what WM_DEVICECHANGE does. */
	void RequestRescan();

	/** Every attached and remembered device, for `apexsim.input.Devices`. */
	void DumpDevicesToLog() const;

	// --- For the device -----------------------------------------------------

	TArray<ApexDirectInput::FSlotRecord>& GetRecords() { return Records; }
	void SaveRecords() const;

	/** The device publishes a new attached set. */
	void NotifyDevicesChanged();

private:
	void LoadRecords();
	FString GetRecordsPath() const;

	/** The slot's short tag: the kind, numbered when two attached devices share it. */
	FString GetDeviceTag(int32 Slot) const;

#if PLATFORM_WINDOWS
	TSharedPtr<FApexDirectInputDevice> Device;
#endif
	TArray<ApexDirectInput::FSlotRecord> Records;
	uint32 DevicesSerial = 0;
	FSimpleMulticastDelegate DevicesChanged;
};
