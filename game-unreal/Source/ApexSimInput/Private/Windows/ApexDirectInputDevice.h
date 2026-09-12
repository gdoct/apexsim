#pragma once

#if PLATFORM_WINDOWS

#include "CoreMinimal.h"
#include "ApexDirectInputTypes.h"
#include "IInputDevice.h"

class FApexSimInputModule;

// Forward declarations of the COM interfaces, so dinput.h and the Windows
// headers stay inside the one .cpp that needs them.
struct IDirectInput8W;

/**
 * Every attached DirectInput game controller, polled into Unreal key events,
 * and force feedback on the ones that can play it.
 *
 * Polled on the game thread (the default device affinity): Tick rescans when
 * asked and runs the force-feedback watchdog, SendControllerEvents reads every
 * device and sends what changed. An axis is sent whenever it moves and every
 * frame it rests away from zero, the way the engine's XInput device treats a
 * stick: a pedal rests at -1, and the player input flushes its key state when
 * a race ends, so the resting value has to keep arriving.
 *
 * Cooperative level is background (a wheel keeps working with another window
 * in front) on a message-only helper window, which is what SDL does too; the
 * game's own window may not exist yet on the first poll. Force feedback needs
 * exclusive access, so a device that can play effects is opened exclusively,
 * falling back to shared (input only) when another program holds it.
 */
class FApexDirectInputDevice : public IInputDevice
{
public:
	FApexDirectInputDevice(const TSharedRef<FGenericApplicationMessageHandler>& InMessageHandler, FApexSimInputModule& InModule);
	virtual ~FApexDirectInputDevice() override;

	// IInputDevice
	virtual void Tick(float DeltaTime) override;
	virtual void SendControllerEvents() override;
	virtual void SetMessageHandler(const TSharedRef<FGenericApplicationMessageHandler>& InMessageHandler) override;
	virtual bool Exec(UWorld* InWorld, const TCHAR* Cmd, FOutputDevice& Ar) override { return false; }
	virtual void SetChannelValue(int32 ControllerId, FForceFeedbackChannelType ChannelType, float Value) override {}
	virtual void SetChannelValues(int32 ControllerId, const FForceFeedbackValues& Values) override {}
	/** Rumble channels are the pad's; a wheel's forces come through SetWheelEffects. */
	virtual bool SupportsForceFeedback(int32 ControllerId) override { return false; }
	virtual void SetDeviceProperty(int32 ControllerId, const FInputDeviceProperty* Property) override;
	/**
	 * Deliberately false: the platform reports "a gamepad is attached" to the
	 * UI from this, and a wheel is not one. It also keeps the engine's pad
	 * rumble channels from being routed here.
	 */
	virtual bool IsGamepadAttached() const override { return false; }

	/** Stops every effect and releases every device. Safe to call twice. */
	void Shutdown();

	const TArray<ApexDirectInput::FDeviceInfo>& GetDevices() const { return DeviceInfos; }
	float GetControlValue(const ApexDirectInput::FControl& Control) const;
	void SetWheelEffects(int32 Slot, const FApexWheelEffects& Effects);
	void RequestRescan(double DelaySeconds);
	void DumpToLog() const;

private:
	struct FJoystick;

	void Rescan();
	/** Opens and configures a device, shared; null for one that is skipped or fails. */
	TUniquePtr<FJoystick> Open(const void* DeviceInstance);

	/**
	 * Takes the device exclusively and loads its effects, which is what playing
	 * forces needs. False while something else holds it — then it is still
	 * read, just silent.
	 */
	bool AcquireForForces(FJoystick& Joystick);

	/** Gives it back: effects unloaded, centring restored, shared again. */
	void ReleaseForces(FJoystick& Joystick);

	void CreateEffects(FJoystick& Joystick);
	void ApplyEffects(FJoystick& Joystick, const FApexWheelEffects& Effects);
	/** Sends every control of a device back to rest, so nothing it held stays held. */
	void ReleaseControls(FJoystick& Joystick);
	void Close(FJoystick& Joystick);
	void PublishDevices();

	TSharedRef<FGenericApplicationMessageHandler> MessageHandler;
	FApexSimInputModule& Module;

	IDirectInput8W* DirectInput = nullptr;
	/** Message-only HWND for the cooperative level. */
	void* HelperWindow = nullptr;

	TArray<TUniquePtr<FJoystick>> Joysticks;
	TArray<ApexDirectInput::FDeviceInfo> DeviceInfos;

	/** When to enumerate next; 0 when nothing is pending. */
	double RescanAt = 0.0;

	/** Which device is playing forces, and when the game last said what. */
	int32 EffectsSlot = INDEX_NONE;
	double EffectsTime = 0.0;
};

#endif // PLATFORM_WINDOWS
