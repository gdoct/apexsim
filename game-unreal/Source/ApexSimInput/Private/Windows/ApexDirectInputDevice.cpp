#include "Windows/ApexDirectInputDevice.h"

#if PLATFORM_WINDOWS

#include "ApexSimInputLog.h"
#include "ApexSimInputModule.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "GenericPlatform/IInputInterface.h"
#include "HAL/PlatformTime.h"

#include "Windows/AllowWindowsPlatformTypes.h"
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include "Windows/HideWindowsPlatformTypes.h"

using namespace ApexDirectInput;

namespace
{
	/** The range every axis is asked to report in; whatever a driver keeps instead is read back. */
	constexpr LONG RequestedAxisMin = 0;
	constexpr LONG RequestedAxisMax = 65535;

	/** Movement below this is the noise of a potentiometer or a hall sensor, not the player. */
	constexpr float AxisSendEpsilon = 1.0f / 8192.0f;

	/** Resting further than this from zero is resent every frame (see the class comment). */
	constexpr float AxisRestBand = 0.02f;

	/** WM_DEVICECHANGE arrives several times for one plug; enumerate once they stop. */
	constexpr double RescanDebounceSeconds = 0.75;

	/** A game that has not updated forces for this long is hitching or gone: let the wheel go. */
	constexpr double EffectsWatchdogSeconds = 0.3;

	/** Frames of failed reads and reacquires before a device is taken for unplugged. */
	constexpr int32 LostPollLimit = 30;

	/** How often to try for exclusive access again while another program holds the wheel. */
	constexpr double ForceRetrySeconds = 3.0;

	const FName RequestDeviceUpdateProperty = TEXT("Request_Device_Update");
	const wchar_t* const HelperWindowClass = L"ApexSimDirectInputHelper";

	const DWORD AxisOffsets[MaxAxes] = {
		DIJOFS_X, DIJOFS_Y, DIJOFS_Z, DIJOFS_RX, DIJOFS_RY, DIJOFS_RZ, DIJOFS_SLIDER(0), DIJOFS_SLIDER(1) };

	LONG AxisReading(const DIJOYSTATE2& State, int32 Axis)
	{
		switch (Axis)
		{
		case 0: return State.lX;
		case 1: return State.lY;
		case 2: return State.lZ;
		case 3: return State.lRx;
		case 4: return State.lRy;
		case 5: return State.lRz;
		case 6: return State.rglSlider[0];
		case 7: return State.rglSlider[1];
		default: return 0;
		}
	}

	FGuid ToGuid(const GUID& In)
	{
		static_assert(sizeof(FGuid) == sizeof(GUID), "GUID and FGuid are both 128 bits");
		FGuid Out;
		FMemory::Memcpy(&Out, &In, sizeof(GUID));
		return Out;
	}

	LONG ToMagnitude(float Value)
	{
		return static_cast<LONG>(FMath::RoundToInt(FMath::Clamp(Value, -1.0f, 1.0f) * DI_FFNOMINALMAX));
	}

	DWORD ToUnsignedMagnitude(float Value)
	{
		return static_cast<DWORD>(FMath::RoundToInt(FMath::Clamp(Value, 0.0f, 1.0f) * DI_FFNOMINALMAX));
	}

	template <typename T>
	void SafeRelease(T*& Interface)
	{
		if (Interface)
		{
			Interface->Release();
			Interface = nullptr;
		}
	}

	DIPROPDWORD MakeDwordProperty(DWORD Value)
	{
		DIPROPDWORD Property = {};
		Property.diph.dwSize = sizeof(DIPROPDWORD);
		Property.diph.dwHeaderSize = sizeof(DIPROPHEADER);
		Property.diph.dwObj = 0;
		Property.diph.dwHow = DIPH_DEVICE;
		Property.dwData = Value;
		return Property;
	}

	/**
	 * An Xbox pad, or anything else XInput already reads: its device path
	 * carries "IG_". The engine's XInput device has it already, with separate
	 * triggers that DirectInput would merge into one axis.
	 */
	bool IsXInputDevice(IDirectInputDevice8W* Device)
	{
		DIPROPGUIDANDPATH Path = {};
		Path.diph.dwSize = sizeof(DIPROPGUIDANDPATH);
		Path.diph.dwHeaderSize = sizeof(DIPROPHEADER);
		Path.diph.dwHow = DIPH_DEVICE;
		return SUCCEEDED(Device->GetProperty(DIPROP_GUIDANDPATH, &Path.diph))
			&& FCString::Stristr(Path.wszPath, TEXT("IG_")) != nullptr;
	}

	BOOL CALLBACK CollectDevice(LPCDIDEVICEINSTANCEW Instance, LPVOID Context)
	{
		static_cast<TArray<DIDEVICEINSTANCEW>*>(Context)->Add(*Instance);
		return DIENUM_CONTINUE;
	}

	const TCHAR* DescribeResult(HRESULT Result)
	{
		switch (Result)
		{
		case DIERR_OTHERAPPHASPRIO:       return TEXT("another program has it exclusively");
		case DIERR_INPUTLOST:             return TEXT("input lost");
		case DIERR_NOTACQUIRED:           return TEXT("not acquired");
		case DIERR_NOTEXCLUSIVEACQUIRED:  return TEXT("not acquired exclusively");
		case DIERR_UNPLUGGED:             return TEXT("unplugged");
		case DIERR_DEVICEFULL:            return TEXT("no room for another effect");
		case DIERR_UNSUPPORTED:           return TEXT("unsupported");
		case DIERR_INVALIDPARAM:          return TEXT("invalid parameter");
		case E_HANDLE:                    return TEXT("bad window handle");
		default:                          return TEXT("failed");
		}
	}
}

struct FApexDirectInputDevice::FJoystick
{
	IDirectInputDevice8W* Device = nullptr;
	FDeviceInfo Info;
	FGuid Instance;

	/** Held exclusively, which force feedback needs. Taken only once forces are asked for. */
	bool bExclusive = false;
	/** When that was last tried, so a wheel another program holds is not hammered. */
	double LastForceAttempt = 0.0;
	int32 FailedPolls = 0;
	/** A reading has arrived since the device was opened; the first one is its rest position. */
	bool bHaveReading = false;

	LONG AxisMin[MaxAxes] = {};
	LONG AxisMax[MaxAxes] = {};
	float Axes[MaxAxes] = {};
	float RestAxes[MaxAxes] = {};
	float SentAxes[MaxAxes] = {};
	bool bAxisSent[MaxAxes] = {};
	bool Buttons[MaxButtons] = {};
	uint8 Hats[MaxHats] = {};

	/** The axis that pushes back: the rim, on a wheel. */
	DWORD ActuatorOffset = DIJOFS_X;
	IDirectInputEffect* ConstantEffect = nullptr;
	IDirectInputEffect* VibrationEffect = nullptr;
	IDirectInputEffect* DamperEffect = nullptr;
	IDirectInputEffect* SpringEffect = nullptr;

	/** What each effect was last sent, so an unchanged one costs no USB traffic. */
	LONG PlayingConstant = 0;
	DWORD PlayingVibrationMagnitude = 0;
	DWORD PlayingVibrationPeriod = 0;
	LONG PlayingDamper = 0;
	LONG PlayingSpring = 0;
	/** False after an acquire was lost: the device dropped its effects, so everything is sent again. */
	bool bEffectsKnown = false;
};

FApexDirectInputDevice::FApexDirectInputDevice(
	const TSharedRef<FGenericApplicationMessageHandler>& InMessageHandler, FApexSimInputModule& InModule)
	: MessageHandler(InMessageHandler)
	, Module(InModule)
{
	const HINSTANCE Instance = GetModuleHandleW(nullptr);

	// Registering twice (a module restarted by live coding) fails harmlessly.
	WNDCLASSEXW WindowClass = {};
	WindowClass.cbSize = sizeof(WindowClass);
	WindowClass.lpfnWndProc = DefWindowProcW;
	WindowClass.hInstance = Instance;
	WindowClass.lpszClassName = HelperWindowClass;
	RegisterClassExW(&WindowClass);

	HelperWindow = CreateWindowExW(0, HelperWindowClass, L"ApexSim DirectInput", 0, 0, 0, 0, 0,
		HWND_MESSAGE, nullptr, Instance, nullptr);
	if (!HelperWindow)
	{
		UE_LOG(LogApexInput, Error, TEXT("DirectInput: could not create the helper window (error %u); wheels are unavailable"),
			GetLastError());
		return;
	}

	const HRESULT Result = DirectInput8Create(Instance, DIRECTINPUT_VERSION, IID_IDirectInput8W,
		reinterpret_cast<void**>(&DirectInput), nullptr);
	if (FAILED(Result))
	{
		DirectInput = nullptr;
		UE_LOG(LogApexInput, Error, TEXT("DirectInput: DirectInput8Create failed (0x%08x); wheels are unavailable"),
			static_cast<uint32>(Result));
		return;
	}

	UE_LOG(LogApexInput, Log, TEXT("DirectInput: ready"));
	RequestRescan(0.0);
}

FApexDirectInputDevice::~FApexDirectInputDevice()
{
	Shutdown();
}

void FApexDirectInputDevice::Shutdown()
{
	for (TUniquePtr<FJoystick>& Joystick : Joysticks)
	{
		Close(*Joystick);
	}
	Joysticks.Reset();
	DeviceInfos.Reset();
	EffectsSlot = INDEX_NONE;

	SafeRelease(DirectInput);

	if (HelperWindow)
	{
		DestroyWindow(static_cast<HWND>(HelperWindow));
		HelperWindow = nullptr;
		UnregisterClassW(HelperWindowClass, GetModuleHandleW(nullptr));
	}
}

void FApexDirectInputDevice::SetMessageHandler(const TSharedRef<FGenericApplicationMessageHandler>& InMessageHandler)
{
	MessageHandler = InMessageHandler;
}

void FApexDirectInputDevice::SetDeviceProperty(int32 ControllerId, const FInputDeviceProperty* Property)
{
	// What the platform application sends every WM_DEVICECHANGE: something was
	// plugged in or pulled out, and it does not say what.
	if (Property && Property->Name == RequestDeviceUpdateProperty)
	{
		RequestRescan(RescanDebounceSeconds);
	}
}

void FApexDirectInputDevice::RequestRescan(double DelaySeconds)
{
	// Debounced: every request pushes the scan back, so a burst of device
	// notifications costs one enumeration.
	RescanAt = FPlatformTime::Seconds() + FMath::Max(DelaySeconds, 0.001);
}

void FApexDirectInputDevice::Tick(float DeltaTime)
{
	const double Now = FPlatformTime::Seconds();

	if (RescanAt > 0.0 && Now >= RescanAt)
	{
		RescanAt = 0.0;
		Rescan();
	}

	if (EffectsSlot != INDEX_NONE && Now - EffectsTime > EffectsWatchdogSeconds)
	{
		for (TUniquePtr<FJoystick>& Joystick : Joysticks)
		{
			if (Joystick->Info.Slot == EffectsSlot)
			{
				ApplyEffects(*Joystick, FApexWheelEffects());
			}
		}
		UE_LOG(LogApexInput, Verbose, TEXT("DirectInput: no force update for %.2f s; wheel released"), Now - EffectsTime);
		EffectsSlot = INDEX_NONE;
	}
}

void FApexDirectInputDevice::Rescan()
{
	if (!DirectInput)
	{
		return;
	}

	const double Started = FPlatformTime::Seconds();

	TArray<DIDEVICEINSTANCEW> Found;
	const HRESULT Result = DirectInput->EnumDevices(DI8DEVCLASS_GAMECTRL, &CollectDevice, &Found, DIEDFL_ATTACHEDONLY);
	if (FAILED(Result))
	{
		UE_LOG(LogApexInput, Warning, TEXT("DirectInput: enumeration failed (0x%08x)"), static_cast<uint32>(Result));
		return;
	}

	bool bChanged = false;

	// Gone, or lost for good: a device that stopped answering is closed and,
	// if it is still attached, opened afresh below.
	for (int32 Index = Joysticks.Num() - 1; Index >= 0; --Index)
	{
		FJoystick& Joystick = *Joysticks[Index];
		const bool bAttached = Found.ContainsByPredicate([&Joystick](const DIDEVICEINSTANCEW& Instance)
		{
			return ToGuid(Instance.guidInstance) == Joystick.Instance;
		});
		if (!bAttached || Joystick.FailedPolls >= LostPollLimit)
		{
			UE_LOG(LogApexInput, Log, TEXT("DirectInput: device %d \"%s\" %s"),
				Joystick.Info.Slot + 1, *Joystick.Info.Name, bAttached ? TEXT("stopped answering; reopening") : TEXT("removed"));
			ReleaseControls(Joystick);
			Close(Joystick);
			Joysticks.RemoveAt(Index);
			bChanged = true;
		}
	}

	uint32 BusySlots = 0;
	for (const TUniquePtr<FJoystick>& Joystick : Joysticks)
	{
		BusySlots |= 1u << Joystick->Info.Slot;
	}

	for (const DIDEVICEINSTANCEW& Instance : Found)
	{
		const FGuid InstanceGuid = ToGuid(Instance.guidInstance);
		const bool bOpen = Joysticks.ContainsByPredicate([&InstanceGuid](const TUniquePtr<FJoystick>& Joystick)
		{
			return Joystick->Instance == InstanceGuid;
		});
		if (bOpen)
		{
			continue;
		}

		TUniquePtr<FJoystick> Joystick = Open(&Instance);
		if (!Joystick)
		{
			continue;
		}

		TArray<FSlotRecord>& Records = Module.GetRecords();
		const int32 Slot = AssignSlot(Records, InstanceGuid, ToGuid(Instance.guidProduct), BusySlots);
		if (Slot == INDEX_NONE)
		{
			UE_LOG(LogApexInput, Warning, TEXT("DirectInput: \"%s\" ignored; all %d device slots are in use"),
				*Joystick->Info.Name, MaxDevices);
			Close(*Joystick);
			continue;
		}

		Joystick->Info.Slot = Slot;
		if (FSlotRecord* Record = Records.FindByPredicate([Slot](const FSlotRecord& Candidate) { return Candidate.Slot == Slot; }))
		{
			Record->Name = Joystick->Info.Name;
			Record->Kind = Joystick->Info.Kind;
		}
		BusySlots |= 1u << Slot;

		const FDeviceInfo& Info = Joystick->Info;
		UE_LOG(LogApexInput, Log,
			TEXT("DirectInput: device %d \"%s\" (%s, %04x:%04x): %d axes, %d buttons, %d hats, %s"),
			Slot + 1, *Info.Name, KindName(Info.Kind), Info.VendorId, Info.ProductId,
			FMath::CountBits(Info.AxisMask), Info.NumButtons, Info.NumHats,
			Info.bCanPlayForces ? TEXT("can play forces") : TEXT("input only"));

		Joysticks.Add(MoveTemp(Joystick));
		bChanged = true;
	}

	if (bChanged)
	{
		Module.SaveRecords();
		Joysticks.Sort([](const TUniquePtr<FJoystick>& A, const TUniquePtr<FJoystick>& B) { return A->Info.Slot < B->Info.Slot; });
		PublishDevices();
	}

	UE_LOG(LogApexInput, Verbose, TEXT("DirectInput: scan took %.1f ms, %d attached"),
		(FPlatformTime::Seconds() - Started) * 1000.0, Joysticks.Num());
}

TUniquePtr<FApexDirectInputDevice::FJoystick> FApexDirectInputDevice::Open(const void* DeviceInstance)
{
	const DIDEVICEINSTANCEW& Instance = *static_cast<const DIDEVICEINSTANCEW*>(DeviceInstance);
	const FString Name = FString(Instance.tszProductName).TrimStartAndEnd();

	IDirectInputDevice8W* Device = nullptr;
	if (FAILED(DirectInput->CreateDevice(Instance.guidInstance, &Device, nullptr)) || !Device)
	{
		UE_LOG(LogApexInput, Warning, TEXT("DirectInput: could not open \"%s\""), *Name);
		return nullptr;
	}

	TUniquePtr<FJoystick> Joystick = MakeUnique<FJoystick>();
	Joystick->Device = Device;
	Joystick->Instance = ToGuid(Instance.guidInstance);
	Joystick->Info.Name = Name;

	if (IsXInputDevice(Device))
	{
		UE_LOG(LogApexInput, Verbose, TEXT("DirectInput: \"%s\" is an XInput device; left to XInput"), *Name);
		Close(*Joystick);
		return nullptr;
	}

	if (FAILED(Device->SetDataFormat(&c_dfDIJoystick2)))
	{
		UE_LOG(LogApexInput, Warning, TEXT("DirectInput: \"%s\" rejected the joystick data format"), *Name);
		Close(*Joystick);
		return nullptr;
	}

	DIDEVCAPS Caps = {};
	Caps.dwSize = sizeof(Caps);
	Device->GetCapabilities(&Caps);
	const bool bCanPlayForces = (Caps.dwFlags & DIDC_FORCEFEEDBACK) != 0;

	// Shared to begin with, always. Playing forces needs exclusive access, and
	// taking that at startup would mean an editor, a second copy of the game or
	// a telemetry tool holding a wheel it is not using.
	const HWND Window = static_cast<HWND>(HelperWindow);
	if (FAILED(Device->SetCooperativeLevel(Window, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND)))
	{
		UE_LOG(LogApexInput, Warning, TEXT("DirectInput: \"%s\" refused a cooperative level"), *Name);
		Close(*Joystick);
		return nullptr;
	}
	Joystick->Info.bCanPlayForces = bCanPlayForces;

	bool bFoundActuator = false;
	for (int32 Axis = 0; Axis < MaxAxes; ++Axis)
	{
		DIDEVICEOBJECTINSTANCEW Object = {};
		Object.dwSize = sizeof(Object);
		if (FAILED(Device->GetObjectInfo(&Object, AxisOffsets[Axis], DIPH_BYOFFSET)))
		{
			continue;
		}

		// Ask for one range everywhere; a driver that keeps its own is read back.
		DIPROPRANGE Range = {};
		Range.diph.dwSize = sizeof(DIPROPRANGE);
		Range.diph.dwHeaderSize = sizeof(DIPROPHEADER);
		Range.diph.dwObj = AxisOffsets[Axis];
		Range.diph.dwHow = DIPH_BYOFFSET;
		Range.lMin = RequestedAxisMin;
		Range.lMax = RequestedAxisMax;
		Device->SetProperty(DIPROP_RANGE, &Range.diph);
		if (FAILED(Device->GetProperty(DIPROP_RANGE, &Range.diph)) || Range.lMax <= Range.lMin)
		{
			continue;
		}

		Joystick->AxisMin[Axis] = Range.lMin;
		Joystick->AxisMax[Axis] = Range.lMax;
		Joystick->Info.AxisMask |= 1u << Axis;

		if (!bFoundActuator && (Object.dwFlags & DIDOI_FFACTUATOR) != 0)
		{
			Joystick->ActuatorOffset = AxisOffsets[Axis];
			bFoundActuator = true;
		}
	}

	// No deadzone or saturation of DirectInput's own: the game's shaping is
	// the only shaping, and a deadzone on a wheel is steering nobody asked for.
	DIPROPDWORD Deadzone = MakeDwordProperty(0);
	Device->SetProperty(DIPROP_DEADZONE, &Deadzone.diph);
	DIPROPDWORD Saturation = MakeDwordProperty(DI_FFNOMINALMAX);
	Device->SetProperty(DIPROP_SATURATION, &Saturation.diph);

	DIPROPDWORD VidPid = MakeDwordProperty(0);
	if (SUCCEEDED(Device->GetProperty(DIPROP_VIDPID, &VidPid.diph)))
	{
		Joystick->Info.VendorId = LOWORD(VidPid.dwData);
		Joystick->Info.ProductId = HIWORD(VidPid.dwData);
	}

	Joystick->Info.NumButtons = FMath::Min(static_cast<int32>(Caps.dwButtons), MaxButtons);
	Joystick->Info.NumHats = FMath::Min(static_cast<int32>(Caps.dwPOVs), MaxHats);

	FDeviceTraits Traits;
	const BYTE Type = GET_DIDEVICE_TYPE(Instance.dwDevType);
	Traits.bDrivingType = Type == DI8DEVTYPE_DRIVING;
	Traits.bGamepadType = Type == DI8DEVTYPE_GAMEPAD;
	Traits.bJoystickType = Type == DI8DEVTYPE_JOYSTICK || Type == DI8DEVTYPE_FLIGHT || Type == DI8DEVTYPE_1STPERSON;
	Traits.bForceFeedback = bCanPlayForces;
	Joystick->Info.Kind = ClassifyDevice(Name, Traits);

	if (Joystick->bExclusive)
	{
		// The driver's centring spring would fight the car's own torque. Both
		// properties can only be set while the device is not acquired.
		DIPROPDWORD AutoCenter = MakeDwordProperty(DIPROPAUTOCENTER_OFF);
		Device->SetProperty(DIPROP_AUTOCENTER, &AutoCenter.diph);
		DIPROPDWORD Gain = MakeDwordProperty(DI_FFNOMINALMAX);
		Device->SetProperty(DIPROP_FFGAIN, &Gain.diph);
	}

	const HRESULT Acquired = Device->Acquire();
	if (FAILED(Acquired))
	{
		// Not fatal: every poll tries again.
		UE_LOG(LogApexInput, Verbose, TEXT("DirectInput: \"%s\" not acquired yet: %s"), *Name, DescribeResult(Acquired));
	}

	return Joystick;
}

bool FApexDirectInputDevice::AcquireForForces(FJoystick& Joystick)
{
	if (Joystick.bExclusive)
	{
		return true;
	}
	if (!Joystick.Info.bCanPlayForces || !Joystick.Device)
	{
		return false;
	}

	// Another program may hold the wheel (a second copy of the game, an editor,
	// a force-feedback tool), so this can fail for as long as that lasts. Try
	// again now and then rather than every frame.
	const double Now = FPlatformTime::Seconds();
	if (Now - Joystick.LastForceAttempt < ForceRetrySeconds)
	{
		return false;
	}
	Joystick.LastForceAttempt = Now;

	const HWND Window = static_cast<HWND>(HelperWindow);
	Joystick.Device->Unacquire();
	HRESULT Result = Joystick.Device->SetCooperativeLevel(Window, DISCL_EXCLUSIVE | DISCL_BACKGROUND);
	if (SUCCEEDED(Result))
	{
		// Both properties can only be set while the device is not acquired. The
		// driver's own centring spring would fight the car's torque.
		DIPROPDWORD AutoCenter = MakeDwordProperty(DIPROPAUTOCENTER_OFF);
		Joystick.Device->SetProperty(DIPROP_AUTOCENTER, &AutoCenter.diph);
		DIPROPDWORD Gain = MakeDwordProperty(DI_FFNOMINALMAX);
		Joystick.Device->SetProperty(DIPROP_FFGAIN, &Gain.diph);
		Result = Joystick.Device->Acquire();
	}

	if (SUCCEEDED(Result))
	{
		Joystick.bExclusive = true;
		CreateEffects(Joystick);
		Joystick.Info.bForcesReady = Joystick.ConstantEffect != nullptr;
		UE_LOG(LogApexInput, Log, TEXT("DirectInput: playing forces on device %d \"%s\"%s"),
			Joystick.Info.Slot + 1, *Joystick.Info.Name,
			Joystick.Info.bForcesReady ? TEXT("") : TEXT(" — but it has no effects to play"));
		// Only when it worked: a wheel another program is holding would
		// otherwise announce a change every few seconds for as long as it does.
		PublishDevices();
	}
	else
	{
		UE_LOG(LogApexInput, Warning, TEXT("DirectInput: \"%s\": %s; driving it without force feedback"),
			*Joystick.Info.Name, DescribeResult(Result));
		ReleaseForces(Joystick);
	}

	return Joystick.Info.bForcesReady;
}

void FApexDirectInputDevice::ReleaseForces(FJoystick& Joystick)
{
	for (IDirectInputEffect** Effect : { &Joystick.ConstantEffect, &Joystick.VibrationEffect, &Joystick.DamperEffect, &Joystick.SpringEffect })
	{
		if (*Effect)
		{
			(*Effect)->Stop();
			(*Effect)->Unload();
			SafeRelease(*Effect);
		}
	}
	Joystick.bEffectsKnown = false;
	Joystick.bExclusive = false;
	Joystick.Info.bForcesReady = false;

	if (Joystick.Device)
	{
		// Back to shared, with the wheel's own centring handed back, so
		// whatever wants it next can have it.
		const HWND Window = static_cast<HWND>(HelperWindow);
		Joystick.Device->Unacquire();
		DIPROPDWORD AutoCenter = MakeDwordProperty(DIPROPAUTOCENTER_ON);
		Joystick.Device->SetProperty(DIPROP_AUTOCENTER, &AutoCenter.diph);
		Joystick.Device->SetCooperativeLevel(Window, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
		Joystick.Device->Acquire();
	}
}

void FApexDirectInputDevice::CreateEffects(FJoystick& Joystick)
{
	// Single-axis effects on the actuator. With one axis the sign of the
	// magnitude is the direction, so the direction array is a formality.
	DWORD Axes[1] = { Joystick.ActuatorOffset };
	LONG Direction[1] = { 0 };

	DIEFFECT Effect = {};
	Effect.dwSize = sizeof(DIEFFECT);
	Effect.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
	Effect.dwDuration = INFINITE;
	Effect.dwGain = DI_FFNOMINALMAX;
	Effect.dwTriggerButton = DIEB_NOTRIGGER;
	Effect.cAxes = 1;
	Effect.rgdwAxes = Axes;
	Effect.rglDirection = Direction;

	auto Create = [&Joystick, &Effect](const GUID& Type, void* Parameters, DWORD Size, const TCHAR* What) -> IDirectInputEffect*
	{
		Effect.cbTypeSpecificParams = Size;
		Effect.lpvTypeSpecificParams = Parameters;
		IDirectInputEffect* Created = nullptr;
		const HRESULT Result = Joystick.Device->CreateEffect(Type, &Effect, &Created, nullptr);
		if (FAILED(Result) || !Created)
		{
			UE_LOG(LogApexInput, Log, TEXT("DirectInput: \"%s\" has no %s effect (%s)"),
				*Joystick.Info.Name, What, DescribeResult(Result));
			return nullptr;
		}
		// Playing from the start, silent: from here on only the parameters change.
		Created->Start(1, 0);
		return Created;
	};

	DICONSTANTFORCE Constant = { 0 };
	Joystick.ConstantEffect = Create(GUID_ConstantForce, &Constant, sizeof(Constant), TEXT("constant force"));

	DIPERIODIC Periodic = { 0, 0, 0, 20000 };
	Joystick.VibrationEffect = Create(GUID_Sine, &Periodic, sizeof(Periodic), TEXT("sine"));

	DICONDITION Condition = { 0, 0, 0, DI_FFNOMINALMAX, DI_FFNOMINALMAX, 0 };
	Joystick.DamperEffect = Create(GUID_Damper, &Condition, sizeof(Condition), TEXT("damper"));
	Joystick.SpringEffect = Create(GUID_Spring, &Condition, sizeof(Condition), TEXT("spring"));

	Joystick.PlayingConstant = 0;
	Joystick.PlayingVibrationMagnitude = 0;
	Joystick.PlayingVibrationPeriod = Periodic.dwPeriod;
	Joystick.PlayingDamper = 0;
	Joystick.PlayingSpring = 0;
	Joystick.bEffectsKnown = true;
}

void FApexDirectInputDevice::ApplyEffects(FJoystick& Joystick, const FApexWheelEffects& Effects)
{
	if (!Joystick.bExclusive)
	{
		return;
	}

	const bool bResendAll = !Joystick.bEffectsKnown;
	bool bLost = false;

	auto Send = [&bLost, bResendAll](IDirectInputEffect* Effect, void* Parameters, DWORD Size)
	{
		DIEFFECT Change = {};
		Change.dwSize = sizeof(DIEFFECT);
		Change.cbTypeSpecificParams = Size;
		Change.lpvTypeSpecificParams = Parameters;

		// NORESTART keeps a running effect running; a device that cannot
		// update one in place says so, and gets a restart instead. After a lost
		// acquire the effect has to be downloaded and started again anyway.
		const DWORD Flags = DIEP_TYPESPECIFICPARAMS | (bResendAll ? DIEP_START : DIEP_NORESTART);
		HRESULT Result = Effect->SetParameters(&Change, Flags);
		if (Result == DIERR_EFFECTPLAYING)
		{
			Result = Effect->SetParameters(&Change, DIEP_TYPESPECIFICPARAMS);
		}
		if (Result == DIERR_NOTEXCLUSIVEACQUIRED || Result == DIERR_NOTACQUIRED || Result == DIERR_INPUTLOST)
		{
			bLost = true;
		}
		return SUCCEEDED(Result);
	};

	if (IDirectInputEffect* Effect = Joystick.ConstantEffect)
	{
		const LONG Magnitude = ToMagnitude(Effects.Constant);
		if (bResendAll || Magnitude != Joystick.PlayingConstant)
		{
			DICONSTANTFORCE Parameters = { Magnitude };
			if (Send(Effect, &Parameters, sizeof(Parameters)))
			{
				Joystick.PlayingConstant = Magnitude;
			}
		}
	}

	if (IDirectInputEffect* Effect = Joystick.VibrationEffect)
	{
		const DWORD Magnitude = ToUnsignedMagnitude(Effects.VibrationAmplitude);
		// A silent vibration keeps its old period: changing it would cost a
		// report for nothing.
		const DWORD Period = Magnitude > 0
			? static_cast<DWORD>(1.0e6f / FMath::Clamp(Effects.VibrationHz, 1.0f, 500.0f))
			: Joystick.PlayingVibrationPeriod;
		// Small steps are not worth a USB report; the period only matters
		// once it has moved by a few percent.
		const bool bMagnitudeMoved = FMath::Abs(static_cast<int32>(Magnitude) - static_cast<int32>(Joystick.PlayingVibrationMagnitude)) > 100
			|| (Magnitude == 0) != (Joystick.PlayingVibrationMagnitude == 0);
		const bool bPeriodMoved = FMath::Abs(static_cast<int32>(Period) - static_cast<int32>(Joystick.PlayingVibrationPeriod))
			> static_cast<int32>(Joystick.PlayingVibrationPeriod / 32);
		if (bResendAll || bMagnitudeMoved || bPeriodMoved)
		{
			DIPERIODIC Parameters = { Magnitude, 0, 0, Period };
			if (Send(Effect, &Parameters, sizeof(Parameters)))
			{
				Joystick.PlayingVibrationMagnitude = Magnitude;
				Joystick.PlayingVibrationPeriod = Period;
			}
		}
	}

	auto SendCondition = [&Send, bResendAll](IDirectInputEffect* Effect, float Coefficient01, LONG& Playing)
	{
		const LONG Coefficient = ToMagnitude(FMath::Clamp(Coefficient01, 0.0f, 1.0f));
		if (Effect && (bResendAll || FMath::Abs(Coefficient - Playing) > 100))
		{
			DICONDITION Parameters = { 0, Coefficient, Coefficient, DI_FFNOMINALMAX, DI_FFNOMINALMAX, 0 };
			if (Send(Effect, &Parameters, sizeof(Parameters)))
			{
				Playing = Coefficient;
			}
		}
	};
	SendCondition(Joystick.DamperEffect, Effects.Damper, Joystick.PlayingDamper);
	SendCondition(Joystick.SpringEffect, Effects.Spring, Joystick.PlayingSpring);

	// Lost mid-way: the next successful poll reacquires, and everything is
	// sent again from scratch.
	Joystick.bEffectsKnown = !bLost;
}

void FApexDirectInputDevice::SetWheelEffects(int32 Slot, const FApexWheelEffects& Effects)
{
	if (Slot != EffectsSlot)
	{
		// Forces move with the steering binding; the device left behind is
		// handed back rather than held silently.
		for (TUniquePtr<FJoystick>& Joystick : Joysticks)
		{
			if (Joystick->Info.Slot == EffectsSlot && Joystick->bExclusive)
			{
				ApplyEffects(*Joystick, FApexWheelEffects());
				ReleaseForces(*Joystick);
				PublishDevices();
			}
		}
		EffectsSlot = Slot;
	}

	EffectsTime = FPlatformTime::Seconds();

	for (TUniquePtr<FJoystick>& Joystick : Joysticks)
	{
		if (Joystick->Info.Slot == Slot)
		{
			// The first frame of forces is what takes the wheel: until a race
			// asks, the device is shared and anything else may have it.
			if (AcquireForForces(*Joystick))
			{
				ApplyEffects(*Joystick, Effects);
			}
			break;
		}
	}
}

void FApexDirectInputDevice::SendControllerEvents()
{
	if (Joysticks.IsEmpty())
	{
		return;
	}

	const IPlatformInputDeviceMapper& Mapper = IPlatformInputDeviceMapper::Get();
	const FPlatformUserId User = Mapper.GetPrimaryPlatformUser();
	const FInputDeviceId InputDevice = Mapper.GetDefaultInputDevice();

	bool bLostOne = false;

	for (TUniquePtr<FJoystick>& JoystickPtr : Joysticks)
	{
		FJoystick& Joystick = *JoystickPtr;
		IDirectInputDevice8W* Device = Joystick.Device;
		const int32 Slot = Joystick.Info.Slot;

		DIJOYSTATE2 State;
		HRESULT Result = Device->Poll();
		if (FAILED(Result))
		{
			// Lost or never acquired: try again, and a device coming back has
			// forgotten its effects.
			Result = Device->Acquire();
			if (SUCCEEDED(Result))
			{
				Joystick.bEffectsKnown = false;
				Result = Device->Poll();
			}
		}
		if (SUCCEEDED(Result))
		{
			Result = Device->GetDeviceState(sizeof(DIJOYSTATE2), &State);
		}
		if (FAILED(Result))
		{
			if (++Joystick.FailedPolls == LostPollLimit)
			{
				UE_LOG(LogApexInput, Log, TEXT("DirectInput: device %d \"%s\" is not answering (%s)"),
					Slot + 1, *Joystick.Info.Name, DescribeResult(Result));
				ReleaseControls(Joystick);
				bLostOne = true;
			}
			continue;
		}
		Joystick.FailedPolls = 0;

		for (int32 Axis = 0; Axis < MaxAxes; ++Axis)
		{
			if (!Joystick.Info.HasAxis(Axis))
			{
				continue;
			}

			const float Value = NormaliseAxis(AxisReading(State, Axis), Joystick.AxisMin[Axis], Joystick.AxisMax[Axis]);
			Joystick.Axes[Axis] = Value;
			if (!Joystick.bHaveReading)
			{
				Joystick.RestAxes[Axis] = Value;
			}

			if (!Joystick.bAxisSent[Axis]
				|| FMath::Abs(Value - Joystick.SentAxes[Axis]) > AxisSendEpsilon
				|| FMath::Abs(Value) > AxisRestBand)
			{
				MessageHandler->OnControllerAnalog(
					MakeKey(FControl{ Slot, EControlKind::Axis, Axis }).GetFName(), User, InputDevice, Value);
				Joystick.SentAxes[Axis] = Value;
				Joystick.bAxisSent[Axis] = true;
			}
		}
		Joystick.bHaveReading = true;

		for (int32 Button = 0; Button < Joystick.Info.NumButtons; ++Button)
		{
			const bool bDown = (State.rgbButtons[Button] & 0x80) != 0;
			if (bDown == Joystick.Buttons[Button])
			{
				continue;
			}
			Joystick.Buttons[Button] = bDown;

			const FName Key = MakeKey(FControl{ Slot, EControlKind::Button, Button }).GetFName();
			if (bDown)
			{
				MessageHandler->OnControllerButtonPressed(Key, User, InputDevice, false);
			}
			else
			{
				MessageHandler->OnControllerButtonReleased(Key, User, InputDevice, false);
			}
		}

		for (int32 Hat = 0; Hat < Joystick.Info.NumHats; ++Hat)
		{
			const uint8 Bits = HatBitsFromPov(State.rgdwPOV[Hat]);
			const uint8 Changed = Bits ^ Joystick.Hats[Hat];
			Joystick.Hats[Hat] = Bits;
			for (int32 Direction = 0; Direction < HatDirections; ++Direction)
			{
				if ((Changed & (1u << Direction)) == 0)
				{
					continue;
				}
				const FName Key = MakeKey(FControl{ Slot, EControlKind::Hat, Hat * HatDirections + Direction }).GetFName();
				if ((Bits & (1u << Direction)) != 0)
				{
					MessageHandler->OnControllerButtonPressed(Key, User, InputDevice, false);
				}
				else
				{
					MessageHandler->OnControllerButtonReleased(Key, User, InputDevice, false);
				}
			}
		}
	}

	if (bLostOne)
	{
		RequestRescan(RescanDebounceSeconds);
	}
}

void FApexDirectInputDevice::ReleaseControls(FJoystick& Joystick)
{
	const IPlatformInputDeviceMapper& Mapper = IPlatformInputDeviceMapper::Get();
	const FPlatformUserId User = Mapper.GetPrimaryPlatformUser();
	const FInputDeviceId InputDevice = Mapper.GetDefaultInputDevice();
	const int32 Slot = Joystick.Info.Slot;
	if (Slot == INDEX_NONE)
	{
		return;
	}

	// Axes go back to where they were when the device arrived rather than to
	// zero: a pedal rests at one end of its travel, and zero is half throttle
	// to a pedal binding. The player input keeps an axis's last value until a
	// new one arrives, so without this a pulled cable would hold the throttle.
	for (int32 Axis = 0; Axis < MaxAxes; ++Axis)
	{
		if (Joystick.bAxisSent[Axis])
		{
			MessageHandler->OnControllerAnalog(
				MakeKey(FControl{ Slot, EControlKind::Axis, Axis }).GetFName(), User, InputDevice, Joystick.RestAxes[Axis]);
			Joystick.Axes[Axis] = Joystick.RestAxes[Axis];
			Joystick.bAxisSent[Axis] = false;
		}
	}
	for (int32 Button = 0; Button < MaxButtons; ++Button)
	{
		if (Joystick.Buttons[Button])
		{
			MessageHandler->OnControllerButtonReleased(
				MakeKey(FControl{ Slot, EControlKind::Button, Button }).GetFName(), User, InputDevice, false);
			Joystick.Buttons[Button] = false;
		}
	}
	for (int32 Hat = 0; Hat < MaxHats; ++Hat)
	{
		for (int32 Direction = 0; Direction < HatDirections; ++Direction)
		{
			if ((Joystick.Hats[Hat] & (1u << Direction)) != 0)
			{
				MessageHandler->OnControllerButtonReleased(
					MakeKey(FControl{ Slot, EControlKind::Hat, Hat * HatDirections + Direction }).GetFName(), User, InputDevice, false);
			}
		}
		Joystick.Hats[Hat] = 0;
	}
}

void FApexDirectInputDevice::Close(FJoystick& Joystick)
{
	for (IDirectInputEffect** Effect : { &Joystick.ConstantEffect, &Joystick.VibrationEffect, &Joystick.DamperEffect, &Joystick.SpringEffect })
	{
		if (*Effect)
		{
			(*Effect)->Stop();
			(*Effect)->Unload();
			SafeRelease(*Effect);
		}
	}

	if (Joystick.Device)
	{
		Joystick.Device->Unacquire();
		if (Joystick.bExclusive)
		{
			// Hand the wheel back the way the driver had it: centring on.
			DIPROPDWORD AutoCenter = MakeDwordProperty(DIPROPAUTOCENTER_ON);
			Joystick.Device->SetProperty(DIPROP_AUTOCENTER, &AutoCenter.diph);
		}
		SafeRelease(Joystick.Device);
	}
}

void FApexDirectInputDevice::PublishDevices()
{
	DeviceInfos.Reset(Joysticks.Num());
	for (const TUniquePtr<FJoystick>& Joystick : Joysticks)
	{
		DeviceInfos.Add(Joystick->Info);
	}
	Module.NotifyDevicesChanged();
}

float FApexDirectInputDevice::GetControlValue(const FControl& Control) const
{
	for (const TUniquePtr<FJoystick>& Joystick : Joysticks)
	{
		if (Joystick->Info.Slot != Control.Slot)
		{
			continue;
		}
		switch (Control.Kind)
		{
		case EControlKind::Axis:
			return Joystick->Info.HasAxis(Control.Index) ? Joystick->Axes[Control.Index] : 0.0f;
		case EControlKind::Button:
			return Control.Index < MaxButtons && Joystick->Buttons[Control.Index] ? 1.0f : 0.0f;
		case EControlKind::Hat:
			return Control.Index < MaxHats * HatDirections
				&& (Joystick->Hats[Control.Index / HatDirections] & (1u << (Control.Index % HatDirections))) != 0 ? 1.0f : 0.0f;
		}
	}
	return 0.0f;
}

void FApexDirectInputDevice::DumpToLog() const
{
	UE_LOG(LogApexInput, Display, TEXT("DirectInput: %d device(s) attached%s"), Joysticks.Num(),
		DirectInput ? TEXT("") : TEXT(" (DirectInput unavailable)"));

	for (const TUniquePtr<FJoystick>& Joystick : Joysticks)
	{
		const FDeviceInfo& Info = Joystick->Info;
		FString Axes;
		for (int32 Axis = 0; Axis < MaxAxes; ++Axis)
		{
			if (Info.HasAxis(Axis))
			{
				Axes += FString::Printf(TEXT(" %s=%+.3f"), AxisName(Axis), Joystick->Axes[Axis]);
			}
		}
		FString Pressed;
		for (int32 Button = 0; Button < Info.NumButtons; ++Button)
		{
			if (Joystick->Buttons[Button])
			{
				Pressed += FString::Printf(TEXT(" B%d"), Button + 1);
			}
		}

		UE_LOG(LogApexInput, Display, TEXT("  %d \"%s\" %s %04x:%04x instance %s"),
			Info.Slot + 1, *Info.Name, KindName(Info.Kind), Info.VendorId, Info.ProductId,
			*Joystick->Instance.ToString(EGuidFormats::DigitsWithHyphensInBraces));
		UE_LOG(LogApexInput, Display, TEXT("     axes:%s  buttons %d%s  hats %d  %s%s"),
			*Axes, Info.NumButtons, *Pressed, Info.NumHats,
			Joystick->bExclusive ? TEXT("exclusive") : TEXT("shared"),
			Info.bForcesReady ? TEXT(", playing forces")
				: (Info.bCanPlayForces ? TEXT(", can play forces") : TEXT("")));
		if (Joystick->bExclusive)
		{
			UE_LOG(LogApexInput, Display, TEXT("     playing: constant %d, sine %u @ %.1f Hz, damper %d, spring %d%s"),
				Joystick->PlayingConstant, Joystick->PlayingVibrationMagnitude,
				Joystick->PlayingVibrationPeriod > 0 ? 1.0e6f / Joystick->PlayingVibrationPeriod : 0.0f,
				Joystick->PlayingDamper, Joystick->PlayingSpring,
				Info.Slot == EffectsSlot ? TEXT(" (driven by the game)") : TEXT(""));
		}
	}
}

#endif // PLATFORM_WINDOWS
