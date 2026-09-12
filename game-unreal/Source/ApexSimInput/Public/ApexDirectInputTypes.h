#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"

/**
 * The DirectInput layer's vocabulary: which controls exist as keys, how an
 * attached device keeps its slot from one run to the next, and what a
 * wheelbase is asked to play.
 *
 * Every control on every device is an ordinary FKey ("DInput1_X",
 * "DInput2_Button7", "DInput1_Hat1Up"), registered with EKeys at startup and
 * fed through the engine's message handler. That is the whole integration:
 * Enhanced Input maps them, the controls screen captures and names them and
 * the settings slot saves them exactly as it does a keyboard key, with no
 * second input path to keep in step.
 *
 * The number in the key is a device SLOT, not an enumeration index: a slot
 * belongs to one physical device (by DirectInput instance GUID) and is
 * remembered in Saved/ApexInputDevices.json, so plugging in a button box does
 * not move the pedals' bindings onto it.
 *
 * Everything in this header is platform-independent and pure, so the rules
 * are covered by `ApexSim.Input.DirectInput.*` tests without a wheel on the
 * desk. The Windows code that talks to the hardware is FApexDirectInputDevice.
 */
namespace ApexDirectInput
{
	/** Devices held at once: a base, pedals, a shifter, a handbrake and spares. */
	inline constexpr int32 MaxDevices = 8;
	/** DIJOYSTATE2's axes: X, Y, Z, RX, RY, RZ and two sliders. */
	inline constexpr int32 MaxAxes = 8;
	inline constexpr int32 MaxButtons = 128;
	inline constexpr int32 MaxHats = 4;
	/** A hat is four keys, one per direction; a diagonal holds two of them. */
	inline constexpr int32 HatDirections = 4;

	enum class EControlKind : uint8
	{
		Axis,
		Button,
		Hat,
	};

	/** One control on one device: what a DirectInput key's name spells. */
	struct FControl
	{
		/** 0-based device slot; INDEX_NONE for a key that is not a DirectInput one. */
		int32 Slot = INDEX_NONE;
		EControlKind Kind = EControlKind::Axis;
		/** Axis 0..7 (X, Y, Z, RX, RY, RZ, S1, S2), button 0..127, or hat * 4 + direction (up, right, down, left). */
		int32 Index = 0;

		bool IsValid() const { return Slot != INDEX_NONE; }
		bool operator==(const FControl& Other) const
		{
			return Slot == Other.Slot && Kind == Other.Kind && Index == Other.Index;
		}
	};

	/** The key for a control. Invalid for anything out of range. */
	APEXSIMINPUT_API FKey MakeKey(const FControl& Control);

	/** The control a key names; an invalid control for any other key. */
	APEXSIMINPUT_API FControl ParseKey(const FKey& Key);

	inline bool IsDirectInputKey(const FKey& Key) { return ParseKey(Key).IsValid(); }

	inline bool IsAxisKey(const FKey& Key)
	{
		const FControl Control = ParseKey(Key);
		return Control.IsValid() && Control.Kind == EControlKind::Axis;
	}

	/** "X", "RZ", "S1". */
	APEXSIMINPUT_API const TCHAR* AxisName(int32 Axis);

	/** "X", "B12", "H1↑": a control's name without its device. */
	APEXSIMINPUT_API FString ControlName(const FControl& Control);

	/** Adds every slot's keys to EKeys. Once, from the module's startup. */
	void RegisterKeys();

	// --- Devices ----------------------------------------------------------------

	/** What a device is for, which is what its bindings are called after. */
	enum class EDeviceKind : uint8
	{
		Wheel,
		Pedals,
		Shifter,
		Handbrake,
		ButtonBox,
		Joystick,
		Gamepad,
		Other,
	};

	/** What DirectInput's device type says, reduced to what classification uses. */
	struct FDeviceTraits
	{
		bool bDrivingType = false;
		bool bGamepadType = false;
		bool bJoystickType = false;
		bool bForceFeedback = false;
	};

	/**
	 * The name wins over the type: pedal sets, shifters and handbrakes all
	 * report themselves as "driving" devices, and several direct-drive bases
	 * report as joysticks. A device that can play forces is a wheel unless its
	 * name says otherwise.
	 */
	APEXSIMINPUT_API EDeviceKind ClassifyDevice(const FString& ProductName, const FDeviceTraits& Traits);

	/** "WHEEL", "PEDALS": the prefix of a binding chip. */
	APEXSIMINPUT_API const TCHAR* KindTag(EDeviceKind Kind);

	/** "Wheel", "Pedals": for the device cards. */
	APEXSIMINPUT_API const TCHAR* KindName(EDeviceKind Kind);

	/** An attached device, as the rest of the game sees it. */
	struct FDeviceInfo
	{
		int32 Slot = INDEX_NONE;
		/** DirectInput's product name, e.g. "FANATEC CSL DD". */
		FString Name;
		EDeviceKind Kind = EDeviceKind::Other;
		uint16 VendorId = 0;
		uint16 ProductId = 0;

		/** The hardware can play effects. Whether this game may is another question. */
		bool bCanPlayForces = false;

		/**
		 * This game holds the device exclusively and its effects are loaded.
		 *
		 * Only ever true of the wheel the steering is bound to, and only once a
		 * race has asked for forces: exclusive access is taken as late as
		 * possible, because it is exclusive — an editor sitting open would
		 * otherwise keep the wheel from the game it just launched.
		 */
		bool bForcesReady = false;
		/** A bit per axis the device actually has, X first. */
		uint8 AxisMask = 0;
		int32 NumButtons = 0;
		int32 NumHats = 0;

		bool HasAxis(int32 Axis) const { return Axis >= 0 && Axis < MaxAxes && (AxisMask & (1u << Axis)) != 0; }
	};

	/** A device that has held a slot. Persisted, most recently attached last. */
	struct FSlotRecord
	{
		int32 Slot = INDEX_NONE;
		/** One physical device on this machine. */
		FGuid Instance;
		/** The model: the same for two identical wheels, or one wheel on another USB port. */
		FGuid Product;
		FString Name;
		EDeviceKind Kind = EDeviceKind::Other;
	};

	/**
	 * The slot for a device that has just attached, updating Records to match.
	 *
	 * In order: the slot this very device held before; else the slot of the
	 * same model that is not attached now (the wheel moved to another USB port,
	 * which on some devices changes the instance GUID); else the lowest slot
	 * nothing has ever held; else the slot of the device attached longest ago.
	 * `BusySlots` has a bit per slot already taken this run, which no rule may
	 * hand out. INDEX_NONE when every slot is busy.
	 */
	APEXSIMINPUT_API int32 AssignSlot(TArray<FSlotRecord>& Records, const FGuid& Instance, const FGuid& Product, uint32 BusySlots);

	/** Records as JSON, for Saved/ApexInputDevices.json. */
	APEXSIMINPUT_API FString RecordsToJson(const TArray<FSlotRecord>& Records);

	/** The inverse; malformed entries are dropped rather than failing the file. */
	APEXSIMINPUT_API TArray<FSlotRecord> RecordsFromJson(const FString& Json);

	// --- Readings ---------------------------------------------------------------

	/** A raw reading over the axis's reported range, as -1..1. */
	APEXSIMINPUT_API float NormaliseAxis(int32 Raw, int32 Min, int32 Max);

	/**
	 * A hat's reading (hundredths of a degree clockwise from up, or centred) as
	 * a bit per direction: up 1, right 2, down 4, left 8. A diagonal is two.
	 */
	APEXSIMINPUT_API uint8 HatBitsFromPov(uint32 Pov);
}

/**
 * What a wheelbase is asked to play this frame.
 *
 * Fractions of the device's own peak rather than newton-metres, so the same
 * numbers suit a 2 Nm gear drive and a 25 Nm direct drive; the player's
 * strength settings are applied before they get here. ApexFfb::MixWheel
 * produces them from the server's DriverFeedback.
 */
struct FApexWheelEffects
{
	/** -1..1: the steering-column torque. Positive turns the rim clockwise, to the right. */
	float Constant = 0.0f;

	/** 0..1 at VibrationHz, on top of the torque: curbs, grass, ABS, hits. */
	float VibrationAmplitude = 0.0f;
	float VibrationHz = 0.0f;

	/** 0..1: resistance in proportion to how fast the rim turns. */
	float Damper = 0.0f;

	/** 0..1: a pull back to centre, for when there is no car to supply one. */
	float Spring = 0.0f;
};
