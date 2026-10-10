#pragma once

#include "CoreMinimal.h"

/**
 * A track guide (docs/content/track-guide.md): a corner-by-corner walk round a
 * circuit, played on the client from two files `apexsim-replay guide`
 * writes offline, `<Stem>.<Class>.guide.json` (this) and the recording
 * beside it, `<Stem>.<Class>.guide.apxs` (an ordinary spectator stream).
 *
 * Pure data and parsing: no world, no files. Every time is seconds from the
 * recording's first frame (what FApexReplayClip::SampleAt takes); every
 * position is the SERVER frame (metres, +X down the start straight, +Y left,
 * +Z up). Every string is a display name: the file never carries a real
 * circuit's or corner's trademark.
 */

/** A camera the file names: a tripod with an eye and a point it looks at. */
struct APEXSIM_API FApexGuideCamera
{
	FString Name;
	/** "fixed"; the car-relative cameras are the client's own and never in the file. */
	FString Kind;
	FVector EyeM = FVector::ZeroVector;
	FVector LookM = FVector::ZeroVector;
	float FovDeg = 50.0f;
};

/** What the guide's car (car 0) does through one corner, and where to watch it. */
struct APEXSIM_API FApexGuideCorner
{
	/** The guide stop's place in the lap, from 1 (the "n / N" progress). */
	int32 Number = 0;
	/** The real-style turn numbers the stop covers (a chicane is two turns); both Number when the file has none. */
	int32 TurnFrom = 0;
	int32 TurnTo = 0;
	/** The display name, or "Turn N" / "Turns N-M". */
	FString Name;
	/** "left", "right", "left-right" or "right-left" (a chicane). */
	FString Direction;
	float EntryM = 0.0f;
	float ApexM = 0.0f;
	float ExitM = 0.0f;
	float TurnDeg = 0.0f;
	float MinRadiusM = 0.0f;

	/** The loop at normal speed. */
	double FromS = 0.0;
	double ToS = 0.0;
	/** The part of it shown again in slow motion, at SlowRate. */
	double SlowFromS = 0.0;
	double SlowToS = 0.0;
	float SlowRate = 0.25f;
	/** Car 0 at the apex; negative when the file has none. */
	double ApexS = -1.0;

	float MinSpeedKph = 0.0f;
	int32 ApexGear = 0;
	float EntrySpeedKph = 0.0f;
	float ExitSpeedKph = 0.0f;
	/** Metres before the apex braking starts; meaningless when bFlatOut or !bHasBrake. */
	float BrakeM = 0.0f;
	bool bHasBrake = false;
	bool bFlatOut = false;
	float ElevationChangeM = 0.0f;
	/** Signed: positive leans into the corner. */
	float BankingDeg = 0.0f;

	/** Generated lines, then the hand-written ones (`<Stem>.guide.yml`). */
	TArray<FString> Gotchas;
	TArray<FString> Notes;
	TArray<FApexGuideCamera> Cameras;
};

/** The circuit's facts, for the overview card. Zero / empty where the file has nothing. */
struct APEXSIM_API FApexGuideTrack
{
	FString Stem;
	FString TrackId;
	FString DisplayName;
	FString Description;
	FString Country;
	FString City;
	FString Category;
	int32 YearBuilt = 0;
	float LengthM = 0.0f;
	bool bHasAltitude = false;
	float AltitudeM = 0.0f;
	float ElevationMinM = 0.0f;
	float ElevationMaxM = 0.0f;
	float ClimbM = 0.0f;
	/** Turns in the lap (real-style numbering; a guide stop may cover two). */
	int32 Corners = 0;
	int32 Left = 0;
	int32 Right = 0;
	/** "clockwise" or "anticlockwise"; empty when unknown. */
	FString Direction;
	float LongestStraightM = 0.0f;
	int32 DrsZones = 0;
	float LapTimeS = 0.0f;
	float TopSpeedKph = 0.0f;
	TArray<FString> Notes;
};

/** The whole guide file. */
struct APEXSIM_API FApexTrackGuide
{
	int32 Version = 0;
	/** car.toml `class` (`F1`, `GT3`, ...): logic keys on it; screens show DisplayClass. */
	FString Class;
	FString DisplayClass;
	/** The recording's file name, beside the guide. */
	FString Recording;
	int64 SourceCrc = 0;
	FString CarId;
	FString CarFolder;
	FString CarName;
	float CarGapS = 0.0f;
	FApexGuideTrack Track;
	double OverviewTimeS = 0.0;
	TArray<FApexGuideCamera> OverviewCameras;
	TArray<FApexGuideCorner> Corners;
};

namespace ApexTrackGuide
{
	/** The `.guide.json` suffix every guide file ends in. */
	inline const TCHAR* JsonSuffix() { return TEXT(".guide.json"); }

	/**
	 * Read a guide. Unknown keys are ignored and optional ones may be missing;
	 * false with a reason only when the text is not JSON, has no `version`
	 * (or one this client is too old for) or names no recording. A camera
	 * without both an eye and a look point is dropped, and a corner's clip is
	 * put in order (from before to, the slow part inside it) rather than
	 * refused.
	 */
	APEXSIM_API bool Parse(const FString& Json, FApexTrackGuide& Out, FString& OutError);

	/** `Spa.GT3.guide.json` -> ("Spa", "GT3"); false for any other name. */
	APEXSIM_API bool ParseFileName(const FString& FileName, FString& OutStem, FString& OutClass);

	/**
	 * Which of a track's guides to open: the preferred class (the car picked
	 * on the create screen) when the track has it, else F1, Hypercar, LMP2,
	 * GT3, else the first of the rest by name. Empty when there are none.
	 * Case-insensitive; returns the spelling in `Available`.
	 */
	APEXSIM_API FString ChooseClass(const TArray<FString>& Available, const FString& Preferred);

	// --- What the cards say (pure, for the tests) ------------------------------

	/** "RIGHT", "LEFT", "LEFT-RIGHT" / "RIGHT-LEFT" (a chicane), or empty. */
	APEXSIM_API FString DirectionLabel(const FString& Direction);
	/** "TURN 3", or "TURNS 3–4" for a stop that covers more than one turn. */
	APEXSIM_API FString TurnLabel(const FApexGuideCorner& Corner);
	/** "Clockwise" / "Anticlockwise", or empty. */
	APEXSIM_API FString TrackDirectionLabel(const FString& Direction);
	/** "Flat out", or "Brake 120 m before the apex". Empty when the file says nothing. */
	APEXSIM_API FString BrakeText(const FApexGuideCorner& Corner);
	/** "2:18.400". */
	APEXSIM_API FString LapTimeText(float Seconds);
	/** A speed in km/h shown in the player's units: "72 km/h" or "45 mph". */
	APEXSIM_API FString SpeedText(float Kph, bool bMetric);
	/** A distance: "7.004 km" past a kilometre, else "420 m" (metric); miles / feet otherwise. */
	APEXSIM_API FString DistanceText(float Metres, bool bMetric);
	/** A signed height: "+8 m", "-12 m" (or feet). */
	APEXSIM_API FString HeightText(float Metres, bool bMetric, bool bSigned);
	/** "Notes first, then gotchas": the lines the corner card lists. */
	APEXSIM_API TArray<FString> CornerLines(const FApexGuideCorner& Corner);
}
