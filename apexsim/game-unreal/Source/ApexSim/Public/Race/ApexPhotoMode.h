#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"

/**
 * Photo mode (docs/game/spectator.md, "Photo mode"): a free camera over a
 * watched race, a replay held still, with a lens (focal length, focus,
 * aperture), exposure compensation and a high-resolution shot. The pure
 * part, so the rules are tested without a world (`ApexSim.Photo.*`): the
 * lens and its steps, how the camera flies and orbits, the keys, the file
 * name. The race director owns the camera it drives
 * (AApexRaceDirector::BeginPhotoMode); UApexPhotoModeWidget owns the input
 * and the panel.
 */
namespace ApexPhoto
{
	/** The sensor the focal lengths are for: full frame, 36 mm wide. */
	inline constexpr float SensorWidthMm = 36.0f;

	/** The rows of the panel, in order. */
	enum class ESetting : uint8
	{
		FocalLength,
		/** Focus distance; 0 focuses on the watched car wherever it is. */
		Focus,
		/** f-number; 0 is no depth of field at all. */
		Aperture,
		Exposure,
		/** The shot's size: the screen times 1, 2 or 4. */
		Resolution,
		Count,
	};

	/** What the photo camera's lens and the shot are set to. */
	struct FLens
	{
		float FocalLengthMm = 35.0f;
		/** Metres; 0: on the watched car. */
		float FocusM = 0.0f;
		/** 0: off. */
		float Fstop = 0.0f;
		/** Stops over or under the sky's own exposure. */
		float ExposureEv = 0.0f;
		/** The shot is this many times the screen's size each way. */
		int32 Resolution = 2;
	};

	/** Horizontal field of view in degrees of a focal length on the 36 mm sensor. */
	APEXSIM_API float FovForFocalLength(float FocalLengthMm);
	/** The focal length a horizontal field of view stands for, clamped to the lens's range. */
	APEXSIM_API float FocalLengthForFov(float FovDeg);

	/** The lens's range, and the steps the panel moves through. */
	inline constexpr float MinFocalLengthMm = 12.0f;
	inline constexpr float MaxFocalLengthMm = 600.0f;
	inline constexpr float MinExposureEv = -4.0f;
	inline constexpr float MaxExposureEv = 4.0f;

	/** The next focal length of the panel's steps (12 ... 600 mm) `Direction` way from `Mm`; a value between steps goes to the neighbouring one. */
	APEXSIM_API float StepFocalLength(float Mm, int32 Direction);
	/** Auto (0), then 1 m to 300 m. */
	APEXSIM_API float StepFocus(float FocusM, int32 Direction);
	/** Off (0), then f/1.4 to f/22 in full stops. */
	APEXSIM_API float StepAperture(float Fstop, int32 Direction);
	/** A third of a stop, -4 to +4 EV. */
	APEXSIM_API float StepExposure(float Ev, int32 Direction);
	/** 1x, 2x, 4x. */
	APEXSIM_API int32 StepResolution(int32 Multiplier, int32 Direction);

	/** One row of the panel a step `Direction` way. */
	APEXSIM_API void StepSetting(FLens& Lens, ESetting Setting, int32 Direction);

	/** The row's name and its value as the panel shows them: "FOCAL LENGTH", "35 mm"; "FOCUS", "AUTO"; "APERTURE", "f/2.8"... */
	APEXSIM_API const TCHAR* SettingName(ESetting Setting);
	APEXSIM_API FString DescribeSetting(const FLens& Lens, ESetting Setting);

	/** The row `Direction` from `Setting`, wrapping. */
	APEXSIM_API ESetting StepRow(ESetting Setting, int32 Direction);

	// --- The camera ----------------------------------------------------------------

	/** What the hands are doing this frame. */
	struct FInput
	{
		/** Forward, right, up: each -1..1 (a stick, a key, a trigger). */
		FVector Move = FVector::ZeroVector;
		/** Yaw right and pitch up, -1..1: turned at LookRateDegPerS. */
		FVector2D Look = FVector2D::ZeroVector;
		/** Yaw right and pitch up in degrees, applied as they are (the mouse). */
		FVector2D LookDeltaDeg = FVector2D::ZeroVector;
		/** Held: five times as fast, or a quarter as fast. */
		bool bFast = false;
		bool bSlow = false;
	};

	inline constexpr float MoveSpeedCmPerS = 800.0f;
	inline constexpr float FastFactor = 5.0f;
	inline constexpr float SlowFactor = 0.25f;
	inline constexpr float LookRateDegPerS = 90.0f;
	/** How far the camera may wander from the watched car. */
	inline constexpr float MaxRangeCm = 15000.0f;
	/** The orbit's nearest. */
	inline constexpr float MinOrbitCm = 250.0f;
	/**
	 * The farthest an orbit starts: one taken up from a wide TV shot comes in
	 * along the same line to this, or it sweeps the scenery (a tree line)
	 * round a car eighty metres off. The wheel zooms it out again.
	 */
	inline constexpr float MaxOrbitStartCm = 1200.0f;
	/** Clearance kept over the ground under the camera. */
	inline constexpr float GroundClearanceCm = 25.0f;
	inline constexpr float MaxPitchDeg = 89.0f;

	/**
	 * The photo camera: a free camera (flies where it looks, up and down in
	 * the world) or an orbit round the watched car (the stick turns round it,
	 * forward closes in, up and down raise and lower the orbit's angle).
	 */
	struct FCamera
	{
		FVector LocationCm = FVector::ZeroVector;
		float YawDeg = 0.0f;
		float PitchDeg = 0.0f;
		bool bOrbit = false;
		float OrbitDistanceCm = 600.0f;
		float OrbitYawDeg = 0.0f;
		float OrbitPitchDeg = 10.0f;
	};

	APEXSIM_API FRotator Rotation(const FCamera& Camera);

	/** A free camera at a pose (where photo mode starts: wherever the watch camera was). */
	APEXSIM_API FCamera FreeAt(const FVector& LocationCm, const FRotator& Rotation);

	/** Switch between free and orbit, keeping the camera where it is: the orbit is taken round `PivotCm`. */
	APEXSIM_API void SetOrbit(FCamera& Camera, bool bOrbit, const FVector& PivotCm);

	/**
	 * One frame: turn, then move (free) or orbit round `PivotCm`, kept within
	 * MaxRangeCm of it. The caller keeps it above the ground (KeepAboveGround).
	 */
	APEXSIM_API void Advance(FCamera& Camera, const FInput& Input, float DeltaSeconds, const FVector& PivotCm);

	/** Lift the camera to GroundClearanceCm over `GroundZ` if it is lower. True when it moved. */
	APEXSIM_API bool KeepAboveGround(FCamera& Camera, double GroundZ);

	/**
	 * What the orbit turns round and the focus follows: `body` (the middle of
	 * the car, INDEX_NONE) or a wheel, `fl` `fr` `rl` `rr` (its hub; the
	 * ApexWheels::EWheel order), for a tread's close-up. False for any other
	 * name (`apexsim.photo wheel <name>`).
	 */
	APEXSIM_API bool SubjectFromName(const FString& Name, int32& OutWheel);

	// --- The keys --------------------------------------------------------------------

	/** A press in photo mode. */
	enum class ECommand : uint8
	{
		None,
		TakeShot,
		/** Back to the watch view. */
		Exit,
		/** Free camera, or round the watched car. */
		Orbit,
		/** Hide the panel and the hints (the HUD is hidden throughout). */
		HidePanel,
		PreviousRow,
		NextRow,
		Decrease,
		Increase,
		/** Pause or play the replay underneath. */
		PlayPause,
		/** Back to where photo mode began. */
		Reset,
		/** The focal length a step shorter or longer, whichever row is picked. */
		ZoomOut,
		ZoomIn,
	};

	/**
	 * What a press does: Enter / pad A the shot, Escape, Backspace, P / pad B
	 * out, O / pad Y orbit, H / pad View the panel, up and down / D-pad up and
	 * down the row, left and right / D-pad left and right its value, Space /
	 * pad X the replay's pause, R / R3 back to the start, - and = / LB and RB
	 * the focal length. Held keys (MoveAxis) are not commands.
	 */
	APEXSIM_API ECommand CommandFor(const FKey& Key);

	/**
	 * A held key's share of the move: W / S forward and back, A / D left and
	 * right, E / Q up and down; I / K and J / L look. False for any other key.
	 */
	APEXSIM_API bool HeldAxisFor(const FKey& Key, FVector& OutMove, FVector2D& OutLook);

	// --- The shot --------------------------------------------------------------------

	/** The shot's multiplier: `Wanted`, halved until the shot fits `MaxDimension` (never under 1). */
	APEXSIM_API int32 FitMultiplier(FIntPoint ViewportSize, int32 Wanted, int32 MaxDimension = 16384);

	/** `ApexSim_<stem>_<yyyymmdd>_<hhmmss>_<n>x` (no extension: the engine adds .png); the stem's odd characters dropped. */
	APEXSIM_API FString MakeShotName(const FDateTime& When, const FString& TrackStem, int32 Multiplier);

	/** Saved/Screenshots/ApexSim, absolute. */
	APEXSIM_API FString ShotFolder();
}
